#include <iostream>
#include "submodlib/constraint.hpp"
#include "submodlib/submod_function.hpp"

// 枚举所有大小不超过 r 的子集合并检查是否全为真
inline bool DFS(const std::vector<size_t>& universe, size_t r, size_t start, std::vector<size_t>& current_set, auto check_func) {
    if (!check_func(current_set)) {
        return false;
    }
    if (current_set.size() == r) {
        return true;
    }
    for (size_t i = start; i < universe.size(); ++i) {
        current_set.push_back(universe[i]);
        if (!DFS(universe, r, i + 1, current_set, check_func)) {
            return false;
        }
        current_set.pop_back();
    }
    return true;
}

inline bool BF_check(submodlib::SubmodularFunction auto& func, submodlib::MatroidConstraint<size_t> auto constraint, std::vector<size_t>& best_subset) {
    std::vector<size_t> universe = func.universe();
    auto n = std::size(universe);
    auto dynamic_constraint = constraint.init_constraint();
    auto r = dynamic_constraint.rank();

    auto check_func = [&](const std::vector<size_t>& subset) {
        auto dynamic_func = func.init_dynamic();
        auto constraint_copy = dynamic_constraint;
        for (const auto& e : subset) {
            if (!constraint_copy.can_add(e)) {
                return true;  // 不满足约束的集合不参与检查
            }
            constraint_copy.add_element(e);
            dynamic_func.add_element(e);
        }
        auto value = func.evaluate(subset);
        auto d_value = dynamic_func.current_value();
        auto best_value = func.evaluate(best_subset);
        auto flag =  std::abs(value - d_value) < 1e-6 && value <= best_value + 1e-6;
        if(!flag) {
            std::cerr << "BF_check failed for subset: {";
            for (const auto& e : subset) {
                std::cerr << e << " ";
            }
            std::cerr << "}, value: " << value << ", dynamic value: " << d_value << ", best value: " << best_value << "\n";
        }
        return flag;
    };

    // 枚举所有大小不超过 r 的子集合
    std::vector<size_t> current_set;
    return DFS(universe, r, 0, current_set, check_func);
}