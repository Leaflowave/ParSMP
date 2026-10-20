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

#include "../utils/solver_args.hpp"
#include "parlay/delayed.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "submodlib/constraint.hpp"
#include "submodlib/solver.hpp"
using namespace submodlib;

struct COVGraph {
    int n, m;
    parlay::sequence<double> weights;
    parlay::sequence<size_t> adj;
    parlay::sequence<size_t> adj_idx;

    auto operator[](size_t i) const { return adj.cut(adj_idx[i], adj_idx[i + 1]); }
    auto operator[](size_t i) { return adj.cut(adj_idx[i], adj_idx[i + 1]); }
};

class COVFunction {
   public:
    using ElementType = size_t;
    using ResultType = double;

    class DynamicCOVFunction {
       public:
        DynamicCOVFunction(const COVGraph& graph) : graph_(graph), covered_(graph.m, false) {}

        void add_element(const ElementType& e) {
            covered_weight_ +=
                parlay::delayed::reduce(parlay::delayed::map_maybe(graph_[e], [&](size_t v) -> std::optional<double> {
                    if (!covered_[v]) {
                        covered_[v] = true;
                        return graph_.weights[v];
                    }
                    return std::nullopt;
                }));
            selected_elements_.push_back(e);
        }
        [[nodiscard]] ResultType marginal_gain(const ElementType& e) const {
            return parlay::delayed::reduce(parlay::delayed::map_maybe(graph_[e], [&](size_t v) -> std::optional<double> {
                if (!covered_[v]) {
                    return graph_.weights[v];
                }
                return std::nullopt;
            }));
        }
        [[nodiscard]] ResultType current_value() const { return covered_weight_; }
        [[nodiscard]] auto selected_elements() const { return std::ranges::subrange(selected_elements_); }

       private:
        const COVGraph& graph_;
        parlay::sequence<bool> covered_;
        double covered_weight_ = 0.0;
        parlay::sequence<size_t> selected_elements_;
    };
    using DynamicType = DynamicCOVFunction;

    COVFunction(COVGraph graph) : graph_(std::move(graph)) {}

    [[nodiscard]] ResultType evaluate(const std::vector<ElementType>& S) const {
        parlay::sequence<bool> covered(graph_.m, false);
        double covered_weight = 0.0;
        for (const auto& e : S) {
            for (const auto& v : graph_[e]) {
                if (!covered[v]) {
                    covered[v] = true;
                    covered_weight += graph_.weights[v];
                }
            }
        }
        return covered_weight;
    }

    [[nodiscard]] auto universe() const {
        return std::views::iota(0u, static_cast<size_t>(graph_.n)) | std::ranges::to<std::vector<ElementType>>();
    }
    [[nodiscard]] bool has_trivial_upper_bound() const { return true; }

    [[nodiscard]] ResultType trivial_upper_bound() const {
        return parlay::reduce(graph_.weights);
    }

    [[nodiscard]] DynamicCOVFunction init_dynamic() const { return DynamicCOVFunction(graph_); }

   private:
    COVGraph graph_;
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

COVFunction COV_read_from_file(const std::string& file_path) {
    auto lines = read_lines(file_path);
    auto n = split(lines[1], ',').size();
    auto m = n + 1;

    COVGraph graph{.n = static_cast<int>(n), .m = static_cast<int>(m)};
    graph.weights.reserve(m);
    // 第零行是权重
    auto tokens = split(lines[0], ',');
    for (size_t i = 0; i < m; ++i) {
        graph.weights.push_back(std::stod(tokens[i]));
    }

    std::vector<std::vector<size_t>> temp_adj(n);
    for (size_t j = 1; j <= m; ++j) {
        auto tokens = split(lines[j], ',');
        for (size_t i = 0; i < n; ++i) {
            if (std::stod(tokens[i]) > 0) {
                temp_adj[i].push_back(j - 1);
            }
        }
    }

    graph.adj_idx.reserve(n + 1);
    graph.adj.reserve(
        std::accumulate(temp_adj.begin(), temp_adj.end(), 0, [](size_t sum, const auto& v) { return sum + v.size(); }));
    for (size_t i = 0; i < n; ++i) {
        graph.adj_idx.push_back(graph.adj.size());
        graph.adj.insert(graph.adj.end(), temp_adj[i].begin(), temp_adj[i].end());
    }
    graph.adj_idx.push_back(graph.adj.size());

    return COVFunction(std::move(graph));
}

int main(int argc, char** argv) {
    auto [k, config] = get_solver_config(argc, argv);
    
    std::string input_file_path = global_parser.get("--input");
    auto function = COV_read_from_file(input_file_path);
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