#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <numeric>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "parlay/delayed.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "submodlib/constraint.hpp"
#include "submodlib/knapsack_solver.hpp"
#include "submodlib/solver.hpp"
#include "submodlib/submod_function.hpp"
#include "../utils/solver_args.hpp"
using namespace submodlib;

struct Graph {
    int n, m;
    parlay::sequence<size_t> adj;
    parlay::sequence<size_t> adj_idx;

    auto operator[](size_t i) const { return adj.cut(adj_idx[i], adj_idx[i + 1]); }
    auto operator[](size_t i) { return adj.cut(adj_idx[i], adj_idx[i + 1]); }
    // std::span<const size_t> operator[](size_t i) const { return {&adj[adj_idx[i]], adj_idx[i + 1] - adj_idx[i]}; }
    // std::span<size_t> operator[](size_t i) { return {&adj[adj_idx[i]], adj_idx[i + 1] - adj_idx[i]}; }
};

// 部分支配集
class DOMFunction {
   public:
    using ElementType = size_t;
    using ResultType = double;

    class DynamicDOMFunction {
       public:
        DynamicDOMFunction(const Graph& graph) : graph_(graph), dominated_(graph.n, false) {}

        void add_element(const ElementType& e) {
            dominated_count_ += parlay::delayed::reduce(parlay::delayed::map_maybe(graph_[e], [&](size_t v) -> std::optional<size_t> {
                if (!dominated_[v]) {
                    dominated_[v] = true;
                    return 1;
                }
                return std::nullopt;
            }));
            selected_elements_.push_back(e);
        }
        [[nodiscard]] ResultType marginal_gain(const ElementType& e) const {
            return parlay::delayed::reduce(parlay::delayed::map_maybe(graph_[e], [&](size_t v) -> std::optional<size_t> {
                if (!dominated_[v]) {
                    return 1;
                }
                return std::nullopt;
            }));
        }
        [[nodiscard]] ResultType current_value() const { return dominated_count_; }
        [[nodiscard]] auto selected_elements() const { return std::ranges::subrange(selected_elements_); }

       private:
        const Graph& graph_;
        parlay::sequence<char> dominated_;
        size_t dominated_count_ = 0;
        parlay::sequence<size_t> selected_elements_;
    };
    using DynamicType = DynamicDOMFunction;

    DOMFunction(Graph graph) : graph_(std::move(graph)) {}

    [[nodiscard]] ResultType evaluate(const std::vector<ElementType>& S) const {
        parlay::sequence<char> dominated(graph_.n, false);
        size_t dominated_count = 0;
        for (const auto& e : S) {
            for (const auto& v : graph_[e]) {
                if (!dominated[v]) {
                    dominated[v] = true;
                    dominated_count++;
                }
            }
        }
        return dominated_count;
    }

    [[nodiscard]] auto universe() const {
        return std::views::iota(0u, static_cast<size_t>(graph_.n)) | std::ranges::to<std::vector<ElementType>>();
    }
    [[nodiscard]] bool has_trivial_upper_bound() const { return true; }

    [[nodiscard]] ResultType trivial_upper_bound() const { return graph_.n; }

    [[nodiscard]] DynamicDOMFunction init_dynamic() const { return DynamicDOMFunction(graph_); }

   private:
    Graph graph_;
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

DOMFunction DOM_read_from_file(const std::string& file_path) {
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

    return DOMFunction(std::move(graph));
}

int main(int argc, char** argv) {
    auto [k, config] = get_solver_config(argc, argv);

    std::string input_file_path = global_parser.get("--input");
    auto function = DOM_read_from_file(input_file_path);
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