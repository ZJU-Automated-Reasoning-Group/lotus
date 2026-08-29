#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lotus::datalog {

// Lattice values are copied before joinMut is invoked and committed only after
// an epoch-wide merge succeeds. Implementations must nevertheless obey the
// usual join laws (associativity, commutativity, and idempotence); otherwise
// fixed-point and parallel results are not defined. joinMut may throw, but it
// must not mutate objects other than *this or the supplied candidate.

template <typename T> class MinLattice {
public:
  MinLattice() = default;
  MinLattice(T value) : value_(std::move(value)) {}

  const T &value() const { return value_; }
  T &value() { return value_; }

  bool joinMut(const MinLattice &other) {
    if (!(other.value_ < value_))
      return false;
    value_ = other.value_;
    return true;
  }

  bool meetMut(const MinLattice &other) {
    if (!(value_ < other.value_))
      return false;
    value_ = other.value_;
    return true;
  }

  friend bool operator==(const MinLattice &lhs, const MinLattice &rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const MinLattice &lhs, const MinLattice &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const MinLattice &lhs, const MinLattice &rhs) {
    return lhs.value_ < rhs.value_;
  }
  friend MinLattice operator+(const MinLattice &lhs, const T &rhs) {
    return MinLattice(lhs.value_ + rhs);
  }
  friend MinLattice operator+(const T &lhs, const MinLattice &rhs) {
    return MinLattice(lhs + rhs.value_);
  }

private:
  T value_{};
};

template <typename T> class MaxLattice {
public:
  MaxLattice() = default;
  MaxLattice(T value) : value_(std::move(value)) {}

  const T &value() const { return value_; }
  T &value() { return value_; }

  bool joinMut(const MaxLattice &other) {
    if (!(value_ < other.value_))
      return false;
    value_ = other.value_;
    return true;
  }

  bool meetMut(const MaxLattice &other) {
    if (!(other.value_ < value_))
      return false;
    value_ = other.value_;
    return true;
  }

  friend bool operator==(const MaxLattice &lhs, const MaxLattice &rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const MaxLattice &lhs, const MaxLattice &rhs) {
    return !(lhs == rhs);
  }
  friend bool operator<(const MaxLattice &lhs, const MaxLattice &rhs) {
    return lhs.value_ < rhs.value_;
  }
  friend MaxLattice operator+(const MaxLattice &lhs, const T &rhs) {
    return MaxLattice(lhs.value_ + rhs);
  }
  friend MaxLattice operator+(const T &lhs, const MaxLattice &rhs) {
    return MaxLattice(lhs + rhs.value_);
  }

private:
  T value_{};
};

template <typename T> class SetLattice {
public:
  SetLattice() = default;
  SetLattice(std::set<T> values) : values_(std::move(values)) {}
  SetLattice(std::initializer_list<T> values) : values_(values) {}

  const std::set<T> &values() const { return values_; }

  bool joinMut(const SetLattice &other) {
    const std::size_t old_size = values_.size();
    values_.insert(other.values_.begin(), other.values_.end());
    return values_.size() != old_size;
  }

  bool meetMut(const SetLattice &other) {
    std::set<T> intersection;
    std::set_intersection(values_.begin(), values_.end(), other.values_.begin(),
                          other.values_.end(),
                          std::inserter(intersection, intersection.end()));
    if (intersection == values_)
      return false;
    values_ = std::move(intersection);
    return true;
  }

  friend bool operator==(const SetLattice &lhs, const SetLattice &rhs) {
    return lhs.values_ == rhs.values_;
  }
  friend bool operator!=(const SetLattice &lhs, const SetLattice &rhs) {
    return !(lhs == rhs);
  }

private:
  std::set<T> values_;
};

template <typename Lattice> class DualLattice {
public:
  DualLattice() = default;
  explicit DualLattice(Lattice value) : value_(std::move(value)) {}

  const Lattice &value() const { return value_; }
  Lattice &value() { return value_; }

  bool joinMut(const DualLattice &other) {
    return value_.meetMut(other.value_);
  }

  bool meetMut(const DualLattice &other) {
    return value_.joinMut(other.value_);
  }

  friend bool operator==(const DualLattice &lhs, const DualLattice &rhs) {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const DualLattice &lhs, const DualLattice &rhs) {
    return !(lhs == rhs);
  }

private:
  Lattice value_{};
};

template <typename... Lattices> class ProductLattice {
public:
  ProductLattice() = default;
  explicit ProductLattice(std::tuple<Lattices...> values)
      : values_(std::move(values)) {}
  explicit ProductLattice(Lattices... values) : values_(std::move(values)...) {}

  const std::tuple<Lattices...> &values() const { return values_; }
  std::tuple<Lattices...> &values() { return values_; }

  bool joinMut(const ProductLattice &other) {
    return joinImpl(other, std::index_sequence_for<Lattices...>{});
  }

  bool meetMut(const ProductLattice &other) {
    return meetImpl(other, std::index_sequence_for<Lattices...>{});
  }

  friend bool operator==(const ProductLattice &lhs, const ProductLattice &rhs) {
    return lhs.values_ == rhs.values_;
  }
  friend bool operator!=(const ProductLattice &lhs, const ProductLattice &rhs) {
    return !(lhs == rhs);
  }

private:
  template <std::size_t... Is>
  bool joinImpl(const ProductLattice &other, std::index_sequence<Is...>) {
    bool changed = false;
    ((changed = std::get<Is>(values_).joinMut(std::get<Is>(other.values_)) ||
                changed),
     ...);
    return changed;
  }

  template <std::size_t... Is>
  bool meetImpl(const ProductLattice &other, std::index_sequence<Is...>) {
    bool changed = false;
    ((changed = std::get<Is>(values_).meetMut(std::get<Is>(other.values_)) ||
                changed),
     ...);
    return changed;
  }

  std::tuple<Lattices...> values_;
};

template <std::size_t Bound, typename T> class BoundedSetLattice {
public:
  BoundedSetLattice() : values_(std::set<T>{}) {}
  explicit BoundedSetLattice(std::set<T> values) {
    if (values.size() <= Bound)
      values_ = std::move(values);
  }
  BoundedSetLattice(std::initializer_list<T> values)
      : BoundedSetLattice(std::set<T>(values)) {}

  static BoundedSetLattice top() {
    BoundedSetLattice result;
    result.values_.reset();
    return result;
  }
  static BoundedSetLattice bottom() { return BoundedSetLattice(); }
  static BoundedSetLattice singleton(T value) {
    return BoundedSetLattice({std::move(value)});
  }

  bool isTop() const { return !values_; }
  const std::optional<std::set<T>> &values() const { return values_; }

  bool contains(const T &value) const {
    return !values_ || values_->find(value) != values_->end();
  }

  bool joinMut(const BoundedSetLattice &other) {
    if (!values_)
      return false;
    if (!other.values_) {
      values_.reset();
      return true;
    }
    const std::set<T> old = *values_;
    values_->insert(other.values_->begin(), other.values_->end());
    if (values_->size() > Bound) {
      values_.reset();
      return true;
    }
    return *values_ != old;
  }

  bool meetMut(const BoundedSetLattice &other) {
    if (!other.values_)
      return false;
    if (!values_) {
      values_ = other.values_;
      return true;
    }
    std::set<T> intersection;
    std::set_intersection(values_->begin(), values_->end(),
                          other.values_->begin(), other.values_->end(),
                          std::inserter(intersection, intersection.end()));
    if (intersection == *values_)
      return false;
    values_ = std::move(intersection);
    return true;
  }

  friend bool operator==(const BoundedSetLattice &lhs,
                         const BoundedSetLattice &rhs) {
    return lhs.values_ == rhs.values_;
  }
  friend bool operator!=(const BoundedSetLattice &lhs,
                         const BoundedSetLattice &rhs) {
    return !(lhs == rhs);
  }

private:
  std::optional<std::set<T>> values_;
};

template <typename T> class ConstantPropagationLattice {
public:
  enum class Kind { Bottom, Constant, Top };

  ConstantPropagationLattice() = default;

  static ConstantPropagationLattice bottom() {
    return ConstantPropagationLattice();
  }
  static ConstantPropagationLattice constant(T value) {
    ConstantPropagationLattice result;
    result.kind_ = Kind::Constant;
    result.value_ = std::move(value);
    return result;
  }
  static ConstantPropagationLattice top() {
    ConstantPropagationLattice result;
    result.kind_ = Kind::Top;
    return result;
  }

  Kind kind() const { return kind_; }
  const T &value() const { return *value_; }

  bool joinMut(const ConstantPropagationLattice &other) {
    if (other.kind_ == Kind::Bottom || kind_ == Kind::Top)
      return false;
    if (kind_ == Kind::Bottom) {
      *this = other;
      return true;
    }
    if (other.kind_ == Kind::Top || !(*value_ == *other.value_)) {
      kind_ = Kind::Top;
      value_.reset();
      return true;
    }
    return false;
  }

  bool meetMut(const ConstantPropagationLattice &other) {
    if (other.kind_ == Kind::Top || kind_ == Kind::Bottom)
      return false;
    if (kind_ == Kind::Top) {
      *this = other;
      return true;
    }
    if (other.kind_ == Kind::Bottom || !(*value_ == *other.value_)) {
      kind_ = Kind::Bottom;
      value_.reset();
      return true;
    }
    return false;
  }

  friend bool operator==(const ConstantPropagationLattice &lhs,
                         const ConstantPropagationLattice &rhs) {
    return lhs.kind_ == rhs.kind_ && lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const ConstantPropagationLattice &lhs,
                         const ConstantPropagationLattice &rhs) {
    return !(lhs == rhs);
  }

private:
  Kind kind_ = Kind::Bottom;
  std::optional<T> value_;
};

template <typename T> using min_lattice = MinLattice<T>;
template <typename T> using max_lattice = MaxLattice<T>;
template <typename T> using set_lattice = SetLattice<T>;
template <typename T> using dual_lattice = DualLattice<T>;
template <typename... Ts> using product_lattice = ProductLattice<Ts...>;
template <std::size_t Bound, typename T>
using bounded_set_lattice = BoundedSetLattice<Bound, T>;
template <typename T>
using constant_propagation_lattice = ConstantPropagationLattice<T>;

} // namespace lotus::datalog

namespace std {

template <typename T> struct hash<lotus::datalog::MinLattice<T>> {
  std::size_t operator()(const lotus::datalog::MinLattice<T> &value) const {
    return std::hash<T>{}(value.value());
  }
};

template <typename T> struct hash<lotus::datalog::MaxLattice<T>> {
  std::size_t operator()(const lotus::datalog::MaxLattice<T> &value) const {
    return std::hash<T>{}(value.value());
  }
};

template <typename T> struct hash<lotus::datalog::SetLattice<T>> {
  std::size_t operator()(const lotus::datalog::SetLattice<T> &value) const {
    std::size_t seed = 0;
    for (const T &element : value.values()) {
      const std::size_t hash_value = std::hash<T>{}(element);
      seed ^= hash_value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

template <typename T> struct hash<lotus::datalog::DualLattice<T>> {
  std::size_t operator()(const lotus::datalog::DualLattice<T> &value) const {
    return std::hash<T>{}(value.value());
  }
};

template <typename... Ts> struct hash<lotus::datalog::ProductLattice<Ts...>> {
  std::size_t
  operator()(const lotus::datalog::ProductLattice<Ts...> &value) const {
    std::size_t seed = 0;
    std::apply(
        [&](const auto &...element) {
          ((seed ^= std::hash<std::decay_t<decltype(element)>>{}(element) +
                    0x9e3779b9 + (seed << 6) + (seed >> 2)),
           ...);
        },
        value.values());
    return seed;
  }
};

template <std::size_t Bound, typename T>
struct hash<lotus::datalog::BoundedSetLattice<Bound, T>> {
  std::size_t
  operator()(const lotus::datalog::BoundedSetLattice<Bound, T> &value) const {
    if (value.isTop())
      return static_cast<std::size_t>(-1);
    std::size_t seed = 0;
    for (const T &element : *value.values()) {
      const std::size_t element_hash = std::hash<T>{}(element);
      seed ^= element_hash + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }
};

template <typename T>
struct hash<lotus::datalog::ConstantPropagationLattice<T>> {
  std::size_t
  operator()(const lotus::datalog::ConstantPropagationLattice<T> &value) const {
    using Lattice = lotus::datalog::ConstantPropagationLattice<T>;
    std::size_t seed = static_cast<std::size_t>(value.kind());
    if (value.kind() == Lattice::Kind::Constant)
      seed ^= std::hash<T>{}(value.value()) + 0x9e3779b9 + (seed << 6) +
              (seed >> 2);
    return seed;
  }
};

} // namespace std
