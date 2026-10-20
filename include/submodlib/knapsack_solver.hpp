#pragma once

#include "parlay/delayed.h"
#include "parlay/primitives.h"
#include "parlay/sequence.h"
#include "solver.hpp"
#include "submodlib/constraint.hpp"
namespace submodlib {
class KnapsackSolverConfig {
   public:
    ReductionStrategy reduction_strategy = ReductionStrategy::TopK;
    UpperBoundStrategy upper_bound_strategy = UpperBoundStrategy::TopK;
    UpdateStrategy update_strategy = UpdateStrategy::Threshold;
    double max_time = std::numeric_limits<double>::infinity();  // 最大运行时间，单位为秒

    KnapsackSolverConfig() = default;

    KnapsackSolverConfig& set_reduction_strategy(ReductionStrategy strategy) {
        reduction_strategy = strategy;
        return *this;
    }
    KnapsackSolverConfig& set_upper_bound_strategy(UpperBoundStrategy strategy) {
        upper_bound_strategy = strategy;
        return *this;
    }
    KnapsackSolverConfig& set_update_strategy(UpdateStrategy strategy) {
        update_strategy = strategy;
        return *this;
    }
    KnapsackSolverConfig& set_max_time(double t) {
        max_time = t;
        return *this;
    }
};

template <SubmodularFunction F>
    requires std::same_as<std::remove_cvref_t<typename F::ElementType>, size_t>
class KnapsackSolver {
   public:
    using C = KnapsackConstraint;

    KnapsackSolver(F init_func, C constraint)
        : func_(std::move(init_func)), constraint_(constraint), total_timer_("Total Time", false) {}
    KnapsackSolver(F&& init_func, C&& constraint)
        : func_(std::move(init_func)), constraint_(constraint), total_timer_("Total Time", false) {}

    F::ResultType solve(KnapsackSolverConfig config = {}) {
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
            return Candidate{.element = e, .marginal_gain = root_node.dynamic_func.marginal_gain(e), .weight = constraint_.get_weight(e)};
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
        double weight;

        std::partial_ordering operator<=>(const Candidate& other) const {
            return marginal_gain * other.weight <=> other.marginal_gain * weight;
        }
    };
    struct Candidates {
        parlay::sequence<Candidate> data;
        parlay::sequence<typename F::ResultType> suffix_marginal_sums;
        parlay::sequence<typename F::ResultType> suffix_weight_sums;

        [[nodiscard]] size_t size() const { return data.size(); }
        [[nodiscard]] bool empty() const { return data.empty(); }
        void clear() {
            data.clear();
            suffix_marginal_sums.clear();
            suffix_weight_sums.clear();
        }

        [[nodiscard]] F::ResultType tot_marginal_gain() const {
            return suffix_marginal_sums.front() - suffix_marginal_sums.back() + data.front().marginal_gain;
        }

        [[nodiscard]] F::ResultType tot_weight() const {
            return suffix_weight_sums.front() - suffix_weight_sums.back() + data.front().weight;
        }

        void maintain_suffix_sums() {
            if (data.empty()) {
                return;
            }
            auto gains = parlay::delayed_tabulate(data.size(),
                                                  [&](size_t i) { return data[data.size() - 1 - i].marginal_gain; });
            auto weights =
                parlay::delayed_tabulate(data.size(), [&](size_t i) { return data[data.size() - 1 - i].weight; });
            parlay::par_do([&] { suffix_marginal_sums = parlay::scan(gains).first; },
                           [&] { suffix_weight_sums = parlay::scan(weights).first; });
            auto n = suffix_marginal_sums.size();
            parlay::par_do([&] { parlay::reverse_inplace(suffix_marginal_sums, 1024); },
                           [&] { parlay::reverse_inplace(suffix_weight_sums, 1024); });
        }
        void sort() {
            parlay::sort_inplace(data, std::less<>{});
            maintain_suffix_sums();
        }
        Candidate pop_top() {
            auto c = data.back();
            data.pop_back();
            suffix_marginal_sums.pop_back();
            suffix_weight_sums.pop_back();
            return c;
        }
        [[nodiscard]] F::ResultType top_k_sum(double k) const {
            if (k == 0 || data.empty()) {
                return typename F::ResultType{};
            }
            if (k >= data.size()) {
                return tot_marginal_gain();
            }
            k += suffix_weight_sums.back();
            size_t p = std::ranges::partition_point(suffix_weight_sums, [&](const auto& w) { return w > k; }) -
                       suffix_weight_sums.begin();
            return suffix_marginal_sums[p] - suffix_marginal_sums.back() +
                   data[p].marginal_gain * ((k - suffix_weight_sums[p]) / data[p].weight);
        }
        [[nodiscard]] F::ResultType top_k_sum_from_s(double k, size_t s) const {
            if (k == 0 || data.empty()) {
                return typename F::ResultType{};
            }
            const auto rs = suffix_marginal_sums.size() - s;
            const auto& weights = suffix_weight_sums;
            const auto& marginals = suffix_marginal_sums;
            if (weights.front() - weights[rs - 1] + data.front().weight <= k) {
                return marginals.front() - marginals[rs - 1] + data.front().marginal_gain;
            }
            k += weights[rs - 1];
            const auto slice_weights = suffix_weight_sums.head(rs);
            const auto p = std::ranges::partition_point(slice_weights, [&](const auto& w) { return w > k; }) -
                           slice_weights.begin();
            return marginals[p] - marginals[rs - 1] + data[p].marginal_gain * ((k - weights[p]) / data[p].weight);

            // const auto slice_weights = suffix_weight_sums.head(rs);
            // const auto slice_marginals = suffix_marginal_sums.head(rs);
            // if (slice_weights.front() - slice_weights.back() + data.front().weight <= k) {
            //     return slice_marginals.front() - slice_marginals.back() + data.front().marginal_gain;
            // }
            // const auto p = std::ranges::partition_point(slice_weights, [&](const auto& w) { return w > k; }) -
            //                slice_weights.begin();
            // return slice_marginals[p] - slice_marginals.back() +
            //        data[p].marginal_gain * ((k - slice_weights[p]) / data[p].weight);
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
            });
            candidates.sort();
        }

        void update_margin_with_threshold(const F::ResultType& threshold) {
            auto p = std::ranges::partition_point(
                         candidates.data, [&](const Candidate& c) { return c.marginal_gain < threshold * c.weight; }) -
                     candidates.data.begin();
            parlay::parallel_for(p, candidates.data.size(), [&](size_t i) {
                auto& c = candidates.data[i];
                c.marginal_gain = dynamic_func.marginal_gain(c.element);
            });
            candidates.sort();
        }

        [[nodiscard]] F::ResultType upper_bound_top_k() const {
            return upper_bound_top_k(constraint.capacity() - constraint.size());
        }
        [[nodiscard]] F::ResultType upper_bound_top_k(const double k) const {
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
        void reduction_top_k(const F::ResultType& lb, const double k) {
            auto pred = [&](const Candidate& c) {
                if (c.marginal_gain <= 0) {
                    return true;
                }
                if (c.weight > k) {
                    return true;
                }
                if (this->upper_bound_top_k(k - c.weight) + dynamic_func.current_value() + c.marginal_gain <= lb) {
                    return true;
                }
                return false;
            };
            auto remain_flags =
                parlay::delayed_tabulate(candidates.data.size(), [&](size_t i) { return !pred(candidates.data[i]); });
            candidates.data = parlay::pack(candidates.data, remain_flags);
            candidates.maintain_suffix_sums();
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
                if (constraint.capacity() - constraint.size() > 0.) {
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
        const auto min_weight = *parlay::min_element(
            parlay::delayed::map(node.candidates.data, [](const Candidate& c) { return c.weight; }));
        parlay::parallel_for(
            0, pruning_point,
            [&](size_t i) {
                if (node.candidates.data[node.candidates.size() - 1 - i].weight > k || time_exceeded()) {
                    return;
                }
                auto threshold = std::max(
                    0., lb - node.dynamic_func.current_value() - node.candidates.top_k_sum_from_s(k - min_weight, i));
                if (min_weight > 0.0) {
                    threshold /= min_weight;
                } else {
                    threshold = 0.0;
                }
                const auto rs = node.candidates.size() - i;
                const auto datas = node.candidates.data.head(rs);
                const auto re =
                    std::ranges::partition_point(
                        datas, [&](const Candidate& c) { return c.marginal_gain <= threshold * c.weight; }) -
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
            while (!node.constraint.can_add(cand.element) && !node.candidates.empty()) {
                cand = node.pop_top();
            }
            if (!node.constraint.can_add(cand.element)) {
                break;
            }
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
