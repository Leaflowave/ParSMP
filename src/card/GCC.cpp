#include <atomic>
#include <sstream>
#include <string>
#include <vector>

#include "../utils/solver_args.hpp"
#include "parlay/delayed.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "submodlib/constraint.hpp"
#include "submodlib/knapsack_solver.hpp"
#include "submodlib/solver.hpp"
#include "submodlib/submod_function.hpp"
using namespace submodlib;

struct Graph {
    int n, m;
    parlay::sequence<size_t> adj;
    parlay::sequence<size_t> adj_idx;

    auto operator[](size_t i) const { return adj.cut(adj_idx[i], adj_idx[i + 1]); }
    auto operator[](size_t i) { return adj.cut(adj_idx[i], adj_idx[i + 1]); }
};

template <typename T>
struct Matrix {
    size_t n;
    size_t m;
    parlay::sequence<T> data;

    Matrix(size_t n, size_t m) : n(n), m(m), data(n * m, 0.0) {}

    auto operator[](size_t i) const { return data.cut(i * m, (i + 1) * m); }
    auto operator[](size_t i) { return data.cut(i * m, (i + 1) * m); }
};

class GCCFunction {
   public:
    using ElementType = size_t;
    using ResultType = double;

    class DynamicGCCFunction {
       public:
        DynamicGCCFunction(const Matrix<int>& distance_matrix) : distance_matrix_(distance_matrix) {
            min_distances_ = parlay::tabulate(distance_matrix.n,
                                              [&](size_t i) { return *parlay::max_element(distance_matrix_[i]); });
            max_distance_sum = parlay::reduce(min_distances_);
            current_distance_sum = max_distance_sum;
        }

        void add_element(const ElementType& e) {
            parlay::parallel_for(0, min_distances_.size(), [&](size_t i) {
                min_distances_[i] = std::min(min_distances_[i], distance_matrix_[e][i]);
            });
            current_distance_sum = parlay::reduce(min_distances_);
            selected_elements_.push_back(e);
        }
        [[nodiscard]] ResultType marginal_gain(const ElementType& e) const {
            return parlay::reduce(parlay::delayed_tabulate(distance_matrix_.n, [&](size_t i) {
                return std::max(0, min_distances_[i] - distance_matrix_[e][i]);
            }));
        }
        [[nodiscard]] ResultType current_value() const { return max_distance_sum - current_distance_sum; }
        [[nodiscard]] auto selected_elements() const { return selected_elements_; }

       private:
        const Matrix<int>& distance_matrix_;
        parlay::sequence<size_t> selected_elements_;
        parlay::sequence<int> min_distances_;
        int max_distance_sum = 0;
        int current_distance_sum = 0;
    };
    using DynamicType = DynamicGCCFunction;

    auto bfs(size_t start) {
        parlay::sequence<size_t> queue;
        parlay::sequence<int> dist(graph_.n, std::numeric_limits<int>::max());
        auto visited = parlay::tabulate(graph_.n, [](size_t _) { return std::atomic<bool>(false); });
        queue.push_back(start);
        dist[start] = 0;
        visited[start] = true;
        int dis = 0;
        while (!queue.empty()) {
            dis++;
            auto next_edges = parlay::delayed::map(queue, [&](size_t i) { return graph_[i]; });
            auto next_queue = parlay::delayed::flatten(next_edges);
            auto func = [&](size_t j) {
                bool expected = false;
                if (!visited[j] && visited[j].compare_exchange_strong(expected, true)) {
                    dist[j] = dis;
                    return true;
                }
                return false;
            };
            queue = parlay::delayed::to_sequence(parlay::delayed::filter(next_queue, func));
        }
        return dist;
    }

    GCCFunction(Graph graph) : graph_(std::move(graph)), matrix_(graph.n, graph.n) {
        matrix_.data = parlay::delayed::to_sequence(
            parlay::delayed::flatten(parlay::delayed::tabulate(graph_.n, [&](size_t i) { return bfs(i); })));
    }

    // When S is empty, its value is n². If n is greater than approximately 45,000, an overflow may occur.
    [[nodiscard]] ResultType evaluate(const std::vector<ElementType>& S) const {
        auto min_distances = parlay::tabulate(graph_.n, [&](size_t i) {
            if (S.empty()) {
                return *parlay::max_element(matrix_[i]);
            }
            return *parlay::min_element(parlay::delayed::map(S, [&](size_t j) { return matrix_[i][j]; }));
        });
        return parlay::reduce(min_distances);
    }

    [[nodiscard]] auto universe() const {
        return std::views::iota(0u, static_cast<size_t>(graph_.n)) | std::ranges::to<std::vector<ElementType>>();
    }
    [[nodiscard]] bool has_trivial_upper_bound() const { return true; }

    // The trivial upper bound for dynamic GCC function is the sum of the maximum gain of each vertex, which is
    // equivalent to the value of the empty set.
    [[nodiscard]] ResultType trivial_upper_bound() const { return evaluate({}); }

    [[nodiscard]] DynamicGCCFunction init_dynamic() const { return DynamicGCCFunction(matrix_); }

   private:
    Graph graph_;
    Matrix<int> matrix_;
};

std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream stream(str);
    std::string token;

    while (std::getline(stream, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

std::vector<std::string> read_lines(const std::string& file_path) {
    std::ifstream file(file_path);
    if (file.fail()) {
        throw std::runtime_error("Fail when open file " + file_path);
    }

    std::string s;
    std::vector<std::string> lines;
    while (!file.eof()) {
        std::getline(file, s);
        if (s.empty()) {
            continue;
        }
        // trim
        s.erase(s.find_last_not_of(" \n\r\t") + 1);
        lines.emplace_back(std::move(s));
        s.clear();
    }
    return lines;
}

GCCFunction GCC_read_from_file(const std::string& file_path) {
    auto lines = read_lines(file_path);
    std::vector<std::pair<size_t, size_t>> edges;
    size_t n = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto tokens = split(lines[i], ' ');
        auto u = std::stoul(tokens[0]);
        auto v = std::stoul(tokens[1]);
        edges.emplace_back(u, v);
        n = std::max(n, std::max(u, v) + 1);
    }
    size_t m = edges.size() * 2 + n;  // 每条边两条方向 + 每个节点一个自环

    Graph graph{.n = static_cast<int>(n), .m = static_cast<int>(m)};

    std::vector<std::vector<size_t>> temp_adj(n);
    for (size_t i = 0; i < n; ++i) {
        temp_adj[i].push_back(i);  // 自环
    }
    for (const auto& [u, v] : edges) {
        temp_adj[u].push_back(v);
        temp_adj[v].push_back(u);
    }

    graph.adj_idx.reserve(n + 1);
    graph.adj.reserve(
        std::accumulate(temp_adj.begin(), temp_adj.end(), 0, [](size_t sum, const auto& v) { return sum + v.size(); }));
    for (size_t i = 0; i < n; ++i) {
        graph.adj_idx.push_back(graph.adj.size());
        graph.adj.insert(graph.adj.end(), temp_adj[i].begin(), temp_adj[i].end());
    }
    graph.adj_idx.push_back(graph.adj.size());

    return GCCFunction(std::move(graph));
}

int main(int argc, char** argv) {
    auto [k, config] = get_solver_config(argc, argv);

    std::string input_file_path = global_parser.get("--input");
    auto function = GCC_read_from_file(input_file_path);
    submodlib::CardinalityConstraint<size_t> constraint(k);
    submodlib::Solver solver(function, constraint);

    auto result = solver.solve(config);
    assert(std::abs(result - solver.get_best_value()) < 1e-6);

    std::cout << "Best value: " << result << "\n";
    auto solution = solver.get_best_solution();
    std::cout << "Best solution: ";
    for (const auto& e : solution) {
        std::cout << e << " ";
    }
    std::cout << "\n";

    auto json = solver.result_to_json();
    json["input_file"] = input_file_path;
    std::string output_file_path = global_parser.get("--output");
    std::ofstream file(output_file_path);
    file << json.dump(4);
    return 0;
}
