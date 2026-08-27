#pragma once

#include "Dataflow/Datalog/Core/Forward.h"
#include "Dataflow/Datalog/Semantic/SemanticIR.h"

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::datalog::detail {

template <typename T> class TypedColumnStorage final : public ColumnStorage {
public:
  std::size_t size() const override { return values_.size(); }
  void reserve(std::size_t count) override { values_.reserve(count); }

  void append(std::any value) override {
    values_.push_back(std::any_cast<T>(std::move(value)));
  }

  void appendMany(std::vector<std::any> values) override {
    values_.reserve(values_.size() + values.size());
    for (std::any &value : values)
      values_.push_back(std::any_cast<T>(std::move(value)));
  }

  void update(std::size_t row, std::any value) override {
    values_.at(row) = std::any_cast<T>(std::move(value));
  }

  void truncate(std::size_t count) override {
    values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(count),
                  values_.end());
  }

  ValueRef value(std::size_t row) const override {
    return ValueRef::direct(values_.at(row));
  }

  std::any materialize(std::size_t row) const override {
    return std::any(values_.at(row));
  }

  std::unique_ptr<ColumnStorage>
  select(const std::vector<std::size_t> &rows) const override {
    auto result = std::make_unique<TypedColumnStorage<T>>();
    result->values_.reserve(rows.size());
    for (std::size_t row : rows)
      result->values_.push_back(values_.at(row));
    return result;
  }

  std::size_t approximateMemoryBytes() const override {
    return values_.capacity() * sizeof(T);
  }

private:
  std::vector<T> values_;
};

template <> class TypedColumnStorage<bool> final : public ColumnStorage {
public:
  std::size_t size() const override { return values_.size(); }
  void reserve(std::size_t) override {}
  void append(std::any value) override {
    values_.push_back(std::any_cast<bool>(std::move(value)));
  }
  void appendMany(std::vector<std::any> values) override {
    for (std::any &value : values)
      values_.push_back(std::any_cast<bool>(std::move(value)));
  }
  void update(std::size_t row, std::any value) override {
    values_.at(row) = std::any_cast<bool>(std::move(value));
  }
  void truncate(std::size_t count) override {
    values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(count),
                  values_.end());
  }
  ValueRef value(std::size_t row) const override {
    return ValueRef::direct(values_.at(row));
  }
  std::any materialize(std::size_t row) const override {
    return std::any(values_.at(row));
  }
  std::unique_ptr<ColumnStorage>
  select(const std::vector<std::size_t> &rows) const override {
    auto result = std::make_unique<TypedColumnStorage<bool>>();
    for (std::size_t row : rows)
      result->values_.push_back(values_.at(row));
    return result;
  }
  std::size_t approximateMemoryBytes() const override {
    return values_.size() * sizeof(bool);
  }

private:
  std::deque<bool> values_;
};

template <> class TypedColumnStorage<std::string> final : public ColumnStorage {
public:
  std::size_t size() const override { return values_.size(); }
  void reserve(std::size_t count) override { values_.reserve(count); }
  void append(std::any value) override {
    appendValue(std::any_cast<std::string>(std::move(value)));
  }
  void appendMany(std::vector<std::any> values) override {
    values_.reserve(values_.size() + values.size());
    for (std::any &value : values)
      appendValue(std::any_cast<std::string>(std::move(value)));
  }
  void update(std::size_t row, std::any value) override {
    values_.at(row) = intern(std::any_cast<std::string>(std::move(value)));
  }
  void truncate(std::size_t count) override {
    values_.erase(values_.begin() + static_cast<std::ptrdiff_t>(count),
                  values_.end());
  }
  ValueRef value(std::size_t row) const override {
    return ValueRef::direct(dictionary_.at(values_.at(row)));
  }
  std::any materialize(std::size_t row) const override {
    return std::any(dictionary_.at(values_.at(row)));
  }
  std::unique_ptr<ColumnStorage>
  select(const std::vector<std::size_t> &rows) const override {
    auto result = std::make_unique<TypedColumnStorage<std::string>>();
    result->values_.reserve(rows.size());
    for (std::size_t row : rows)
      result->appendValue(dictionary_.at(values_.at(row)));
    return result;
  }
  std::size_t approximateMemoryBytes() const override {
    std::size_t bytes = values_.capacity() * sizeof(std::uint32_t) +
                        dictionary_.capacity() * sizeof(std::string);
    for (const std::string &value : dictionary_)
      bytes += value.capacity();
    return bytes;
  }

private:
  std::uint32_t intern(std::string value) {
    auto found = interned_.find(value);
    if (found != interned_.end())
      return found->second;
    if (dictionary_.size() >=
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
      throw std::length_error("Datalog string dictionary exhausted u32 IDs");
    const std::uint32_t id = static_cast<std::uint32_t>(dictionary_.size());
    dictionary_.push_back(std::move(value));
    interned_.emplace(dictionary_.back(), id);
    return id;
  }
  void appendValue(std::string value) {
    values_.push_back(intern(std::move(value)));
  }

  std::vector<std::uint32_t> values_;
  std::vector<std::string> dictionary_;
  std::unordered_map<std::string, std::uint32_t> interned_;
};

template <typename T> struct IsExpression : std::false_type {};
template <typename T> struct IsExpression<Expr<T>> : std::true_type {};
template <typename T> struct IsExpression<Var<T>> : std::true_type {};

template <typename T, typename = void> struct IsHashable : std::false_type {};

template <typename T>
struct IsHashable<T, std::void_t<decltype(std::declval<std::hash<T> &>()(
                         std::declval<const T &>()))>> : std::true_type {};

template <typename T, typename = void>
struct IsEqualityComparable : std::false_type {};

template <typename T>
struct IsEqualityComparable<T, std::void_t<decltype(std::declval<const T &>() ==
                                                    std::declval<const T &>())>>
    : std::true_type {};

template <typename T, typename = void>
struct IsLessComparable : std::false_type {};

template <typename T>
struct IsLessComparable<T, std::void_t<decltype(std::declval<const T &>() <
                                                std::declval<const T &>())>>
    : std::true_type {};

template <typename T, typename = void> struct HasJoinMut : std::false_type {};

template <typename T>
struct HasJoinMut<T, std::void_t<decltype(std::declval<T &>().joinMut(
                         std::declval<const T &>()))>>
    : std::is_same<decltype(std::declval<T &>().joinMut(
                       std::declval<const T &>())),
                   bool> {};

inline std::vector<VarId> mergeReferences(const std::vector<VarId> &lhs,
                                          const std::vector<VarId> &rhs) {
  std::vector<VarId> result = lhs;
  for (VarId id : rhs) {
    if (std::find(result.begin(), result.end(), id) == result.end())
      result.push_back(id);
  }
  return result;
}

Context *mergeContexts(Context *lhs, Context *rhs);

template <typename T> ColumnType makeColumnType() {
  static_assert(IsHashable<T>::value,
                "Datalog relation columns must provide std::hash<T>");
  static_assert(IsEqualityComparable<T>::value,
                "Datalog relation columns must support operator==");

  ColumnType result;
  result.type = typeid(T);
  result.name = typeid(T).name();
  result.hash = [](const std::any &value) {
    return std::hash<T>{}(std::any_cast<const T &>(value));
  };
  result.equal = [](const std::any &lhs, const std::any &rhs) {
    return std::any_cast<const T &>(lhs) == std::any_cast<const T &>(rhs);
  };
  result.hash_value = [](ValueRef value) {
    return std::hash<T>{}(value.get<T>());
  };
  result.equal_value = [](ValueRef lhs, ValueRef rhs) {
    return lhs.get<T>() == rhs.get<T>();
  };
  if constexpr (IsLessComparable<T>::value) {
    result.less_value = [](ValueRef lhs, ValueRef rhs) {
      return lhs.get<T>() < rhs.get<T>();
    };
  }
  result.make_storage = [] {
    return std::make_unique<TypedColumnStorage<T>>();
  };
  result.properties = FunctionProperties::parallel();
  if constexpr (std::is_floating_point_v<T>) {
    result.validate_key = [](const std::any &value) {
      if (std::isnan(std::any_cast<const T &>(value))) {
        throw std::invalid_argument(
            "NaN may not be used in a Datalog relation key");
      }
    };
  }
  return result;
}

} // namespace lotus::datalog::detail
