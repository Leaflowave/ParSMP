#pragma once
#include <concepts>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

namespace submodlib {

// T 不仅是一个范围，且指定范围返回的类型
template <class T, class E>
concept ElementRange =
    std::ranges::range<T> && std::same_as<std::remove_cvref_t<std::ranges::range_value_t<T>>, std::remove_cvref_t<E>>;

///  动态子模函数接口
/// 动态子模函数维护一个当前集合，通过添加元素来更新状态，从而加速边际增益的计算
/// 动态子模函数必须满足 df(\emptyset) = 0 且 边际增益 >= 0，即单调非减
/// 对于单调次模函数 f，df(S) = f(S) - f(\emptyset)
/// 对于单调超模函数 f, df(S) = f(\emptyset) - f(S)
template <class F, class E, class R>
concept DynamicSubmodularFunction = std::floating_point<R> && requires(F& func, const E& e) {
    // 向当前集合中添加元素 e，维护状态，加速边际增益的计算
    { func.add_element(e) } -> std::same_as<void>;
    // 已经选择的集合
    { func.selected_elements() } -> ElementRange<E>;
    // 计算添加元素 e 的边际增益，应当 >= 0
    { func.marginal_gain(e) } -> std::same_as<R>;
    // 当前集合的函数值
    { func.current_value() } -> std::same_as<R>;
};

template <class F>
concept SubmodularFunction = requires(F& func, const std::vector<typename F::ElementType>& S) {
    typename F::ElementType;
    typename F::ResultType;
    requires std::floating_point<typename F::ResultType>;
    typename F::DynamicType;
    requires DynamicSubmodularFunction<typename F::DynamicType, typename F::ElementType, typename F::ResultType>;

    { func.universe() } -> ElementRange<typename F::ElementType>;
    { func.evaluate(S) } -> std::same_as<typename F::ResultType>;
    { func.init_dynamic() } -> std::same_as<typename F::DynamicType>;
    // 是否支持计算平凡上界
    { func.has_trivial_upper_bound() } -> std::same_as<bool>;
    // 计算一个平凡上界，只在初始化后立即被调用一次
    { func.trivial_upper_bound() } -> std::same_as<typename F::ResultType>;
};

}  // namespace submodlib
