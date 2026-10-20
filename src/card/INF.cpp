#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <ranges>
#include <span>
#include <sstream>
#include <vector>

#include "../utils/solver_args.hpp"
#include "parlay/delayed.h"
#include "parlay/monoid.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "submodlib/constraint.hpp"
#include "submodlib/solver.hpp"
#include "submodlib/submod_function.hpp"
using namespace submodlib;

struct INFGraph {
    size_t n;
    size_t m;
    parlay::sequence<double> probabilities;

    INFGraph(size_t n, size_t m) : n(n), m(m), probabilities(n * m, 0.0) {}
    auto operator[](size_t i) const { return probabilities.cut(i * m, (i + 1) * m); }
    auto operator[](size_t i) { return probabilities.cut(i * m, (i + 1) * m); }
    // std::span<const double> operator[](size_t i) const { return {&probabilities[i * m], m}; }
    // std::span<double> operator[](size_t i) { return {&probabilities[i * m], m}; }
};

class INFFunction {
   public:
    using ElementType = size_t;
    using ResultType = double;

    class DynamicINFFunction {
       public:
        DynamicINFFunction(const INFGraph& graph)
            : graph_(graph), not_prob_(graph_.m, 1.0), is_selected_(graph_.n, false), not_prob_sum_(graph_.m) {}

        void add_element(const ElementType& e) {
            parlay::parallel_for(0, graph_.m, [&](size_t j) { not_prob_[j] *= (1.0 - graph_[e][j]); }, 1024);
            not_prob_sum_ = parlay::reduce(not_prob_);
            selected_elements_.push_back(e);
        }
        [[nodiscard]] ResultType marginal_gain(const ElementType& e) const {
            return parlay::delayed::reduce(
                parlay::delayed::tabulate(graph_.m, [&](size_t j) { return not_prob_[j] * graph_[e][j]; }));
        }
        [[nodiscard]] ResultType current_value() const { return graph_.m - not_prob_sum_; }
        [[nodiscard]] auto selected_elements() const { return std::ranges::subrange(selected_elements_); }

       private:
        const INFGraph& graph_;
        parlay::sequence<ElementType> selected_elements_;
        parlay::sequence<bool> is_selected_;
        parlay::sequence<double> not_prob_;
        ResultType not_prob_sum_;
    };
    using DynamicType = DynamicINFFunction;

   public:
    INFFunction(const INFGraph& graph) : graph_(graph) {}

    [[nodiscard]] ResultType evaluate(const std::vector<ElementType>& S) const {
        return graph_.m - parlay::delayed::reduce(parlay::delayed::tabulate(graph_.m, [&](size_t j) {
                   return parlay::delayed::reduce(
                       parlay::delayed::tabulate(S.size(), [&](size_t i) { return (1.0 - graph_[S[i]][j]); }),
                       parlay::multiplies<double>());
               }));
    }
    [[nodiscard]] auto universe() const {
        return std::views::iota(0u, graph_.n) | std::ranges::to<std::vector<ElementType>>();
    }

    [[nodiscard]] DynamicType init_dynamic() const { return DynamicType{graph_}; }
    [[nodiscard]] bool has_trivial_upper_bound() const { return true; }
    [[nodiscard]] ResultType trivial_upper_bound() const {
        return evaluate(universe() | std::ranges::to<std::vector<ElementType>>());
    }

   private:
    INFGraph graph_;
};

static_assert(submodlib::SubmodularFunction<INFFunction>);

std::vector<std::string> read_lines(const std::string& file_path) {
    std::ifstream file(file_path);
    if (file.fail()) {
        std::print(std::cerr, "Fail when open file {}\n", file_path);
    }

    std::string s;
    std::vector<std::string> lines;
    while (!file.eof()) {
        std::getline(file, s);
        if (s.empty() || std::ranges::all_of(s, isspace) || s[0] == '#') {
            continue;
        }
        // trim
        s.erase(s.find_last_not_of(" \n\r\t") + 1);
        lines.emplace_back(std::move(s));
        s.clear();
    }
    return lines;
}
std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::istringstream stream(str);
    std::string token;

    while (std::getline(stream, token, delimiter)) {
        tokens.push_back(token);
    }

    return tokens;
}

auto INF_read_from_file(const std::string& file_path) {
    auto lines = read_lines(file_path);
    auto n = split(lines[0], ',').size();
    auto m = lines.size();

    INFGraph graph(n, m);
    for (size_t j = 0; j < m; ++j) {
        auto tokens = split(lines[j], ',');
        for (size_t i = 0; i < n; ++i) {
            graph[i][j] = std::stod(tokens[i]);
        }
    }
    return INFFunction(graph);
};

int main(int argc, char** argv) {
    auto [k, config] = get_solver_config(argc, argv);

    std::string input_file_path = global_parser.get("--input");
    auto function = INF_read_from_file(input_file_path);
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
