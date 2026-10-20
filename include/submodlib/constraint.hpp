#pragma once
#include <concepts>
#include <cstddef>
#include <span>
#include <vector>

namespace submodlib {

// 拟阵约束接口
template <class C, class E>
concept DynamicMatroidConstraint = requires(C& constraint, const E& element) {
    // 检查是否可以添加元素，应返回 bool
    { constraint.can_add(element) } -> std::convertible_to<bool>;
    // 添加元素
    { constraint.add_element(element) } -> std::same_as<void>;
    // 获取拟阵的基数，应返回 size_t
    { constraint.rank() } -> std::convertible_to<size_t>;
    // 获取当前集合的大小，应返回 size_t
    { constraint.size() } -> std::convertible_to<size_t>;
};

template <class C, class E>
concept MatroidConstraint = requires(C& context) {
    typename C::DynamicType;
    typename C::ElementType;
    requires std::same_as<std::remove_cvref_t<typename C::ElementType>, std::remove_cvref_t<E>>;
    { context.init_constraint() } -> std::same_as<typename C::DynamicType>;
    requires DynamicMatroidConstraint<typename C::DynamicType, typename C::ElementType>;
};

// 基数约束 example
template <class E>
class CardinalityConstraint {
   public:
    class Instance {
       public:
        Instance(size_t max_size) : max_size_(max_size) {}

        [[nodiscard]] bool can_add(const E& element) const { return cur_size_ < max_size_; }
        void add_element(const E& element) { ++cur_size_; }
        [[nodiscard]] size_t capacity() const { return max_size_; }
        [[nodiscard]] size_t size() const { return cur_size_; }

       private:
        size_t cur_size_ = 0;
        size_t max_size_;
    };

    using ElementType = E;
    using DynamicType = Instance;

    CardinalityConstraint(size_t max_size) : max_size_(max_size) {}

    [[nodiscard]] Instance init_constraint() const { return Instance(max_size_); }

   private:
    size_t max_size_;
};

// 背包约束
class KnapsackConstraint {
   public:
    class Instance {
       public:
        Instance(const std::vector<double>& weights, double capacity) : weights_(weights), capacity_(capacity) {}

        [[nodiscard]] bool can_add(size_t element) const { return cur_weight_ + weights_[element] <= capacity_; }
        void add_element(size_t element) { cur_weight_ += weights_[element]; }
        [[nodiscard]] double capacity() const { return capacity_; }
        [[nodiscard]] double size() const { return cur_weight_; }

       private:
        double cur_weight_ = 0.0;
        double capacity_;
        std::span<const double> weights_;
    };

    using ElementType = size_t;
    using DynamicType = Instance;

    KnapsackConstraint(std::vector<double> weights, double capacity)
        : weights_(std::move(weights)), capacity_(capacity) {}

    [[nodiscard]] Instance init_constraint() const { return {weights_, capacity_}; }
    [[nodiscard]] double get_weight(size_t element) const { return weights_[element]; }

   private:
    std::vector<double> weights_;
    double capacity_;
};

}  // namespace submodlib