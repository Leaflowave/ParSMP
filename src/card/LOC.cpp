#include <algorithm>
#include <cassert>
#include <fstream>
#include <iostream>
#include <numeric>
#include <ranges>
#include <span>
#include <sstream>
#include <vector>

#include "submodlib/constraint.hpp"
#include "submodlib/knapsack_solver.hpp"
#include "submodlib/solver.hpp"
#include "submodlib/submod_function.hpp"
#include "../utils/solver_args.hpp"
using namespace submodlib;

struct LOCGraph {
    using DistanceType = double;
    size_t n, m;
    std::vector<DistanceType> distances;

    LOCGraph(size_t n, size_t m) : n(n), m(m), distances(n * m, 0.0) {}

    std::span<const DistanceType> operator[](size_t i) const { return {&distances[i * m], m}; }
    std::span<DistanceType> operator[](size_t i) { return {&distances[i * m], m}; }
};

class LOCFunction {
   public:
    using ElementType = size_t;
    using ResultType = LOCGraph::DistanceType;
    class DynamicLOCFunction {
       public:
        DynamicLOCFunction(const LOCGraph& graph, const std::vector<LOCGraph::DistanceType>& init_max_gain)
            : graph_(graph), max_gain_(init_max_gain) {}

        void add_element(const ElementType& e) {
            current_val += parlay::delayed::reduce(parlay::delayed::tabulate(graph_.m, [&](size_t j) {
                auto gain = std::max(graph_[e][j] - max_gain_[j], 0.0);
                max_gain_[j] = std::max(max_gain_[j], graph_[e][j]);
                return gain;
            }));
            selected_elements_.push_back(e);
        }
        [[nodiscard]] ResultType marginal_gain(const ElementType& e) const {
            return parlay::delayed::reduce(parlay::delayed::tabulate(graph_.m, [&](size_t j) {
                return std::max(graph_[e][j] - max_gain_[j], 0.0);
            }));
        }
        [[nodiscard]] ResultType current_value() const { return current_val; }
        [[nodiscard]] auto selected_elements() const { return std::ranges::subrange(selected_elements_); }

       private:
        const LOCGraph& graph_;
        std::vector<ElementType> selected_elements_;
        std::vector<LOCGraph::DistanceType> max_gain_;
        ResultType current_val{};
    };
    using DynamicType = DynamicLOCFunction;

   public:
    LOCFunction(LOCGraph&& g) : graph_(std::move(g)), init_max_gain_(graph_.m, 0.0) {}
    [[nodiscard]] std::vector<ElementType> universe() const {
        return std::views::iota(0u, graph_.n) | std::ranges::to<std::vector<ElementType>>();
    }
    [[nodiscard]] ResultType evaluate(const std::vector<ElementType>& S) const {
        auto max_gain = init_max_gain_;
        for (const auto& e : S) {
            for (ElementType j = 0; j < graph_.m; ++j) {
                max_gain[j] = std::max(max_gain[j], graph_[e][j]);
            }
        }
        return std::reduce(max_gain.cbegin(), max_gain.cend());
    }
    [[nodiscard]] DynamicType init_dynamic() const { return {graph_, init_max_gain_}; }
    [[nodiscard]] bool has_trivial_upper_bound() const { return true; }
    [[nodiscard]] ResultType trivial_upper_bound() const {
        ResultType ub = 0;
        for (size_t j = 0; j < graph_.m; ++j) {
            double max_gain = 0.0;
            for (size_t i = 0; i < graph_.n; ++i) {
                if (graph_[i][j] > max_gain) {
                    max_gain = graph_[i][j];
                }
            }
            ub += max_gain;
        }
        return ub;
    }

   private:
    LOCGraph graph_;
    std::vector<LOCGraph::DistanceType> init_max_gain_;
};

static_assert(submodlib::DynamicSubmodularFunction<LOCFunction::DynamicLOCFunction, LOCFunction::ElementType,
                                                   LOCFunction::ResultType>);
static_assert(submodlib::SubmodularFunction<LOCFunction>);

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

auto LOC_read_from_file(const std::string& file_path) {
    auto lines = read_lines(file_path);
    auto n = split(lines[0], ',').size();
    auto m = lines.size();

    LOCGraph graph(n, m);
    for (size_t j = 0; j < m; ++j) {
        auto tokens = split(lines[j], ',');
        for (size_t i = 0; i < n; ++i) {
            graph[i][j] = std::stod(tokens[i]);
        }
    }
    return LOCFunction(std::move(graph));
}

// 18.831
int main(int argc, char** argv) {
    auto [k, config] = get_solver_config(argc, argv);

    std::string input_file_path = global_parser.get("--input");
    auto function = LOC_read_from_file(input_file_path);
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
