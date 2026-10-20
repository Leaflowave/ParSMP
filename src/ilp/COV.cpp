#include <algorithm>
#include <numeric>
#include <span>
#include <vector>
#include <string>
#include <ranges>
#include <fstream>
#include <sstream>

#include <gurobi_c++.h>
#include <gurobi_c.h>
#include <parlay/parallel.h>



struct COVGraph {
    int n, m;
    std::vector<double> weights;
    std::vector<size_t> adj;
    std::vector<size_t> adj_idx;

    std::span<const size_t> operator[](size_t i) const { return {&adj[adj_idx[i]], adj_idx[i + 1] - adj_idx[i]}; }
    std::span<size_t> operator[](size_t i) { return {&adj[adj_idx[i]], adj_idx[i + 1] - adj_idx[i]}; }
};

class COVFunction {
   public:
    using ElementType = size_t;
    using ResultType = double;

    class DynamicCOVFunction {
       public:
        DynamicCOVFunction(const COVGraph& graph) : graph_(graph), covered_(graph.m, false) {}

        void add_element(const ElementType& e) {
            for (const auto& v : graph_[e]) {
                if (!covered_[v]) {
                    covered_[v] = true;
                    covered_weight_ += graph_.weights[v];
                }
            }
            selected_elements_.push_back(e);
        }
        [[nodiscard]] ResultType marginal_gain(const ElementType& e) const {
            double gain = 0.0;
            for (const auto& v : graph_[e]) {
                if (!covered_[v]) {
                    gain += graph_.weights[v];
                }
            }
            return gain;
        }
        [[nodiscard]] ResultType current_value() const { return covered_weight_; }
        [[nodiscard]] auto selected_elements() const { return std::ranges::subrange(selected_elements_); }

       private:
        const COVGraph& graph_;
        std::vector<bool> covered_;
        double covered_weight_ = 0.0;
        std::vector<size_t> selected_elements_;
    };
    using DynamicType = DynamicCOVFunction;

    COVFunction(COVGraph graph) : graph_(std::move(graph)) {}

    [[nodiscard]] ResultType evaluate(const std::vector<ElementType>& S) const {
        std::vector<bool> covered(graph_.m, false);
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
        double ub = 0.0;
        for (size_t j = 0; j < graph_.m; ++j) {
            ub += graph_.weights[j];
        }
        return ub;
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
    auto m = n+1;

    COVGraph graph{.n = static_cast<int>(n), .m = static_cast<int>(m)};
    graph.weights.reserve(m);
    // 第零行是权重
    auto tokens = split(lines[0], ',');
    for (size_t i = 0; i < m; ++i) {
        graph.weights.push_back(std::stod(tokens[i]));
    }

    std::vector<std::vector<size_t>> temp_adj(n);
    for(size_t j = 1; j <= m; ++j) {
        auto tokens = split(lines[j], ',');
        for (size_t i = 0; i < n; ++i) {
            if (std::stod(tokens[i]) > 0) {
                temp_adj[i].push_back(j - 1);
            }
        }
    }
    
    graph.adj_idx.reserve(n + 1);
    graph.adj.reserve(std::accumulate(temp_adj.begin(), temp_adj.end(), 0, [](size_t sum, const auto& v) {
        return sum + v.size();
    }));
    for (size_t i = 0; i < n; ++i) {
        graph.adj_idx.push_back(graph.adj.size());
        graph.adj.insert(graph.adj.end(), temp_adj[i].begin(), temp_adj[i].end());
    }
    graph.adj_idx.push_back(graph.adj.size());

    return COVFunction(std::move(graph));
}