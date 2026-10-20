#pragma once
#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <compare>
#include <concepts>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <ranges>
#include <utility>

#include "constraint.hpp"
#include "magic_enum/magic_enum.hpp"
#include "nlohmann/json.hpp"
#include "parlay/alloc.h"
#include "parlay/delayed.h"
#include "parlay/internal/get_time.h"
#include "parlay/monoid.h"
#include "parlay/parallel.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "parlay/utilities.h"
#include "submod_function.hpp"

namespace submodlib {

constexpr inline double EPS = 1e-9;
enum class ReductionStrategy : uint8_t { None, TopK };
enum class UpperBoundStrategy : uint8_t { None, TopK };
enum class UpdateStrategy : uint8_t { Threshold, All };

class SolverConfig {
   public:
    ReductionStrategy reduction_strategy = ReductionStrategy::TopK;
    UpperBoundStrategy upper_bound_strategy = UpperBoundStrategy::TopK;
    UpdateStrategy update_strategy = UpdateStrategy::Threshold;
    double max_time = std::numeric_limits<double>::infinity();  // 最大运行时间，单位为秒

    SolverConfig() = default;

    SolverConfig& set_reduction_strategy(ReductionStrategy strategy) {
        reduction_strategy = strategy;
        return *this;
    }
    SolverConfig& set_upper_bound_strategy(UpperBoundStrategy strategy) {
        upper_bound_strategy = strategy;
        return *this;
    }
    SolverConfig& set_update_strategy(UpdateStrategy strategy) {
        update_strategy = strategy;
        return *this;
    }
    SolverConfig& set_max_time(double t) {
        max_time = t;
        return *this;
    }
};

template <SubmodularFunction F>
class Solver {
   public:
    using C = CardinalityConstraint<typename F::ElementType>;

    Solver(F init_func, C constraint)
        : func_(std::move(init_func)), constraint_(constraint), total_timer_("Total Time", false) {}
    Solver(F&& init_func, C&& constraint)
        : func_(std::move(init_func)), constraint_(constraint), total_timer_("Total Time", false) {}

    F::ResultType solve(SolverConfig config = {}) {
        reduction_strategy_ = config.reduction_strategy;
        upper_bound_strategy_ = config.upper_bound_strategy;
        update_strategy_ = config.update_strategy;
        max_time_ = config.max_time;
        best_value_ = typename F::ResultType{};
        best_solution_.clear();
        early_stop_.store(false, std::memory_order_relaxed);
        // nodes_visited_ = parlay::sequence<size_t>(parlay::num_workers(), 0);
        nodes_visited_ = parlay::sequence<parlay::padded<size_t>>(parlay::num_workers());
        return impl_solve();
    }

    [[nodiscard]] typename F::ResultType get_best_value() { return func_.evaluate(get_best_solution()); }

    [[nodiscard]] auto get_best_solution() {
        lock.test(std::memory_order_acquire);
        return best_solution_.to_vector();
    }

    [[nodiscard]] bool is_early_stopped() const { return early_stop_.load(std::memory_order_relaxed); }

    nlohmann::json result_to_json() {
        nlohmann::json j;
        j["best_value"] = get_best_value();
        j["best_solution"] = get_best_solution();
        j["nodes_visited"] = parlay::reduce(nodes_visited_).x;
        j["total_time"] = total_timer_.total_time();
        j["reduction_strategy"] = magic_enum::enum_name(reduction_strategy_);
        j["upper_bound_strategy"] = magic_enum::enum_name(upper_bound_strategy_);
        j["update_strategy"] = magic_enum::enum_name(update_strategy_);
        j["early_stopped"] = is_early_stopped();
        j["trivial_upper_bound"] = trival_upper_bound_;

        Node root_node{.dynamic_func = func_.init_dynamic(),
                       .constraint = constraint_.init_constraint(),
                       .candidates = Candidates{}};
        auto universe = func_.universe();
        root_node.candidates.data = parlay::map(universe, [&](const auto& e) {
            return Candidate{.element = e, .marginal_gain = root_node.dynamic_func.marginal_gain(e)};
        });
        root_node.candidates.sort();
        j["root_upper_bound"] = upper_bound(root_node);
        return j;
    }

   private:
    F::ResultType impl_solve() {
        total_timer_.reset();
        total_timer_.start();
        std::cout << "Solver started." << '\n';

        auto universe = func_.universe();
        std::cout << "Initializing root node..." << '\n';
        Node root_node{.dynamic_func = func_.init_dynamic(),
                       .constraint = constraint_.init_constraint(),
                       .candidates = Candidates{}};

        root_node.candidates.data = parlay::map(universe, [&](const auto& e) {
            return Candidate{.element = e, .marginal_gain = root_node.dynamic_func.marginal_gain(e)};
        });
        root_node.candidates.sort();

        seq_greedy(root_node);
        std::cout << "Initial greedy solution value: " << get_best_value() << '\n';
        if (func_.has_trivial_upper_bound()) {
            trival_upper_bound_ = func_.trivial_upper_bound();
            std::cout << "Trivial upper bound: " << trival_upper_bound_ << '\n';
        }
        reduction(root_node);

        std::cout << "Starting search..." << '\n';
        bnb_search(root_node);
        total_timer_.stop();
        if (time_exceeded()) {
            std::cout << "Solver stopped due to time limit.\n";
        } else {
            std::cout << "Solver finished.\n";
        }
        std::cout << "Total time: " << total_timer_.total_time() << " seconds.\n";
        std::cout << "Nodes visited: " << parlay::reduce(nodes_visited_).x << '\n';
        return get_best_value();
    }

    struct Candidate {
        F::ElementType element;
        F::ResultType marginal_gain;
        // bool acc;  // Lazy update

        // 比较运算符，优先比较边际增益，其次比较 acc 标志，update 后的优先级更高
        std::partial_ordering operator<=>(const Candidate& other) const {
            return marginal_gain <=> other.marginal_gain;
            // return std::tie(marginal_gain, acc) <=> std::tie(other.marginal_gain, other.acc);
        }
    };
    struct Candidates {
        parlay::sequence<Candidate> data;
        parlay::sequence<typename F::ResultType> suffix_marginal_sums;

        [[nodiscard]] size_t size() const { return data.size(); }
        [[nodiscard]] bool empty() const { return data.empty(); }
        void clear() {
            data.clear();
            suffix_marginal_sums.clear();
        }
        // void clear_acc() {
        //     parlay::parallel_for(0, data.size(), [&](size_t i) { data[i].acc = false; }, 1024);
        // }

        [[nodiscard]] F::ResultType tot_marginal_gain() const {
            return suffix_marginal_sums.front() - suffix_marginal_sums.back() + data.front().marginal_gain;
        }

        void maintain_suffix_sums() {
            if (data.empty()) {
                return;
            }
            auto gains = parlay::delayed_tabulate(data.size(),
                                                  [&](size_t i) { return data[data.size() - 1 - i].marginal_gain; });
            suffix_marginal_sums = parlay::scan(gains).first;
            auto n = suffix_marginal_sums.size();
            parlay::reverse_inplace(suffix_marginal_sums, 1024);
        }
        void sort() {
            parlay::sort_inplace(data, std::less<>{});
            maintain_suffix_sums();
        }
        Candidate pop_top() {
            auto c = data.back();
            data.pop_back();
            suffix_marginal_sums.pop_back();
            return c;
        }
        [[nodiscard]] F::ResultType top_k_sum(size_t k) const {
            if (k == 0 || data.empty()) {
                return typename F::ResultType{};
            }
            if (k >= data.size()) {
                return tot_marginal_gain();
            }
            size_t p = data.size() - k;
            return suffix_marginal_sums[p] - suffix_marginal_sums.back() + data[p].marginal_gain;
        }
        [[nodiscard]] F::ResultType top_k_sum_from_s(size_t k, size_t s) const {
            if (k == 0 || data.empty()) {
                return typename F::ResultType{};
            }
            const auto rs = suffix_marginal_sums.size() - 1 - s;
            if (k + s >= data.size()) {
                return suffix_marginal_sums.front() - suffix_marginal_sums[rs] + data.front().marginal_gain;
            }
            const auto p = suffix_marginal_sums.size() - k - s;
            return suffix_marginal_sums[p] - suffix_marginal_sums[rs] + data[p].marginal_gain;
        }
    };

    struct Node {
        F::DynamicType dynamic_func;
        C::DynamicType constraint;
        Candidates candidates;

        void add_element(const F::ElementType& e) {
            dynamic_func.add_element(e);
            constraint.add_element(e);
            // candidates.clear_acc();
        }

        Candidate pop_top() { return candidates.pop_top(); }

        void update_margin_all() {
            parlay::parallel_for(0, candidates.data.size(), [&](size_t i) {
                auto& c = candidates.data[i];
                c.marginal_gain = dynamic_func.marginal_gain(c.element);
                // c.acc = true;
            });
            candidates.sort();
        }

        void update_margin_with_threshold(const F::ResultType& threshold) {
            auto p = std::ranges::partition_point(candidates.data,
                                                  [&](const Candidate& c) { return c.marginal_gain < threshold; }) -
                     candidates.data.begin();
            parlay::parallel_for(p, candidates.data.size(), [&](size_t i) {
                auto& c = candidates.data[i];
                c.marginal_gain = dynamic_func.marginal_gain(c.element);
                // c.acc = true;
            });
            candidates.sort();
        }

        [[nodiscard]] F::ResultType upper_bound_top_k() const {
            return upper_bound_top_k(constraint.capacity() - constraint.size());
        }
        [[nodiscard]] F::ResultType upper_bound_top_k(const size_t k) const {
            return dynamic_func.current_value() + candidates.top_k_sum(k);
        }

        void reduction_leq_zero() {
            auto pred = [&](const Candidate& c) { return c.marginal_gain <= 0; };
            auto p = std::ranges::partition_point(candidates.data, pred) - candidates.data.begin();
            candidates.data.erase(candidates.data.begin(), candidates.data.begin() + p);
            candidates.suffix_marginal_sums.erase(candidates.suffix_marginal_sums.begin(),
                                                  candidates.suffix_marginal_sums.begin() + p);
        }

        void reduction_top_k(const F::ResultType& lb) {
            reduction_top_k(lb, constraint.capacity() - constraint.size());
        }
        void reduction_top_k(const F::ResultType& lb, const size_t k) {
            auto threshold = std::max(0., lb - upper_bound_top_k(k - 1));
            auto pred = [&](const Candidate& c) { return c.marginal_gain <= threshold; };
            auto p = std::ranges::partition_point(candidates.data, pred) - candidates.data.begin();
            candidates.data.erase(candidates.data.begin(), candidates.data.begin() + p);
            candidates.suffix_marginal_sums.erase(candidates.suffix_marginal_sums.begin(),
                                                  candidates.suffix_marginal_sums.begin() + p);
        }
    };

   private:
    [[nodiscard]] F::ResultType upper_bound(const Node& node) const { return upper_bound(node, node.constraint); }
    [[nodiscard]] F::ResultType upper_bound(const Node& node, const C::DynamicType& constraint) const {
        switch (upper_bound_strategy_) {
            case UpperBoundStrategy::None:
                return typename F::ResultType{};
            case UpperBoundStrategy::TopK:
                return node.upper_bound_top_k(constraint.capacity() - constraint.size());
        }
        return typename F::ResultType{};
    }

    void reduction(Node& node) { reduction(node, get_cur_lower_bound(), node.constraint); }
    void reduction(Node& node, const auto lb) { reduction(node, lb, node.constraint); }
    void reduction(Node& node, const auto lb, const C::DynamicType& constraint) {
        if (constraint.capacity() == constraint.size()) {
            node.candidates.clear();
            return;
        }
        switch (reduction_strategy_) {
            case ReductionStrategy::None:
                node.reduction_leq_zero();
                break;
            case ReductionStrategy::TopK:
                node.reduction_top_k(lb, constraint.capacity() - constraint.size());
                break;
        }
    }

    void update_margin(Node& node) const { update_margin(node, get_cur_lower_bound(), node.constraint); }
    void update_margin(Node& node, const auto lb) const { update_margin(node, lb, node.constraint); }
    void update_margin(Node& node, const auto lb, const C::DynamicType& constraint) const {
        switch (update_strategy_) {
            case UpdateStrategy::Threshold: {
                auto threshold = std::max(0.0, lb - 2 * EPS - node.dynamic_func.current_value());
                if (constraint.capacity() - constraint.size() > 0) {
                    threshold /= (constraint.capacity() - constraint.size());
                } else {
                    threshold = 0.0;
                }
                node.update_margin_with_threshold(threshold);
                break;
            }
            case UpdateStrategy::All:
                node.update_margin_all();
                break;
        }
    }

   private:
    void bnb_search(Node& node) {
        nodes_visited_[parlay::worker_id()]++;
        if (node.candidates.empty() || prune_by_upper_bound(node)) {
            return;
        }
        const auto lb = get_cur_lower_bound();
        reduction(node, lb);
        if (node.candidates.empty() || prune_by_upper_bound(node)) {
            return;
        }
        const auto k = node.constraint.capacity() - node.constraint.size();
        const auto ids = parlay::delayed::iota(node.candidates.size());
        const auto pruning_point = std::ranges::partition_point(ids,
                                                                [&](size_t i) {
                                                                    return node.candidates.top_k_sum_from_s(k, i) +
                                                                               node.dynamic_func.current_value() >
                                                                           lb;
                                                                }) -
                                   ids.begin();
        parlay::parallel_for(
            0, pruning_point,
            [&](size_t i) {
                if (time_exceeded()) {
                    return;
                }
                const auto threshold =
                    std::max(0., lb - node.dynamic_func.current_value() - node.candidates.top_k_sum_from_s(k - 1, i));
                const auto rs = node.candidates.size() - i;
                const auto datas = node.candidates.data.head(rs);
                const auto re = std::ranges::partition_point(
                                    datas, [&](const Candidate& c) { return c.marginal_gain <= threshold; }) -
                                datas.begin();

                Node new_node{
                    .dynamic_func = node.dynamic_func,
                    .constraint = node.constraint,
                    .candidates =
                        Candidates{
                            .data = node.candidates.data.subseq(re, rs),
                        },
                };
                const auto cand = new_node.candidates.data.back();
                new_node.candidates.data.pop_back();
                new_node.add_element(cand.element);
                maintain_best_solution(new_node);
                const auto m_lb = std::max(lb, new_node.dynamic_func.current_value());
                // will maintain the suffix sums for the new node's candidates
                update_margin(new_node, m_lb);

                bnb_search(new_node);
            },
            1);
    }

   private:
    void seq_greedy(Node node) {
        while (!node.candidates.empty()) {
            auto cand = node.pop_top();
            node.add_element(cand.element);
            update_margin(node);
            reduction(node);
        }
        maintain_best_solution(node);
    }

    [[nodiscard]] bool prune_by_upper_bound(const Node& node) const {
        auto lb = get_cur_lower_bound();
        if (func_.has_trivial_upper_bound() && lb >= trival_upper_bound_) {
            return true;
        }
        if (node.dynamic_func.current_value() + node.candidates.tot_marginal_gain() <= lb) {
            return true;
        }
        if (upper_bound(node) <= lb) {
            return true;
        }
        return false;
    }

    bool time_exceeded() {
        auto flag = early_stop_.load(std::memory_order_relaxed);
        if (!flag && total_timer_.total_time() >= max_time_) {
            early_stop_.store(true, std::memory_order_relaxed);
            return true;
        }
        return early_stop_.load(std::memory_order_relaxed);
    }

   private:
    void maintain_best_solution(const Node& node) {
        const double node_val = node.dynamic_func.current_value();
        if (node_val <= best_value_.load(std::memory_order_relaxed)) {
            return;
        }

        while (lock.test_and_set(std::memory_order_acquire)) {
            _mm_pause();
        }

        if (node_val > best_value_.load(std::memory_order_relaxed)) {
            best_solution_ = parlay::to_sequence(node.dynamic_func.selected_elements());
            best_value_.store(node_val, std::memory_order_relaxed);

            lock.clear(std::memory_order_release);
            std::cout << "New best value: " << node_val << '\n';
            std::cout << "Time: " << total_timer_.total_time() << " seconds\n";
            return;
        }
        lock.clear(std::memory_order_release);
    }

    [[nodiscard]] typename F::ResultType get_cur_lower_bound() const {
        // 加上一个很小的数来避免数值问题
        return best_value_.load(std::memory_order_relaxed) + EPS;
    }

   private:
    F func_;
    C constraint_;

    std::atomic_bool early_stop_ = false;
    // parlay::sequence<size_t> nodes_visited_;
    parlay::sequence<parlay::padded<size_t>> nodes_visited_;
    parlay::internal::timer total_timer_;

    std::atomic_flag lock;
    std::atomic<typename F::ResultType> best_value_{};
    parlay::sequence<typename F::ElementType> best_solution_;

    typename F::ResultType trival_upper_bound_ = typename F::ResultType{};

    double max_time_ = std::numeric_limits<double>::infinity();
    ReductionStrategy reduction_strategy_ = ReductionStrategy::TopK;
    UpperBoundStrategy upper_bound_strategy_ = UpperBoundStrategy::TopK;
    UpdateStrategy update_strategy_ = UpdateStrategy::Threshold;
};

}  // namespace submodlib
