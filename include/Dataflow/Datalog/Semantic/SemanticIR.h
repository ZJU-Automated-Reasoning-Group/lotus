#pragma once

#include <any>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <variant>
#include <vector>

namespace lotus::datalog {

using RelationId = std::size_t;
using VarId = std::size_t;
using ColumnMask = std::size_t;

class ValueRef {
public:
  ValueRef() = default;

  static ValueRef fromAny(const std::any &value) {
    ValueRef result;
    result.dynamic_ = &value;
    result.type_ = value.type();
    return result;
  }

  template <typename T> static ValueRef direct(const T &value) {
    ValueRef result;
    result.data_ = &value;
    result.type_ = typeid(T);
    result.copy_ = [](const void *data) {
      return std::any(*static_cast<const T *>(data));
    };
    return result;
  }

  explicit operator bool() const { return data_ || dynamic_; }
  std::type_index type() const { return type_; }

  template <typename T> const T &get() const {
    if (type_ != typeid(T))
      throw std::bad_any_cast();
    if (dynamic_)
      return std::any_cast<const T &>(*dynamic_);
    return *static_cast<const T *>(data_);
  }

  std::any materialize() const {
    if (dynamic_)
      return *dynamic_;
    if (!data_ || !copy_)
      return {};
    return copy_(data_);
  }

private:
  const void *data_ = nullptr;
  const std::any *dynamic_ = nullptr;
  std::type_index type_ = typeid(void);
  std::any (*copy_)(const void *) = nullptr;
};

class ColumnStorage {
public:
  virtual ~ColumnStorage() = default;

  virtual std::size_t size() const = 0;
  virtual void reserve(std::size_t count) = 0;
  virtual void append(std::any value) = 0;
  virtual void update(std::size_t row, std::any value) = 0;
  virtual void truncate(std::size_t count) = 0;
  virtual ValueRef value(std::size_t row) const = 0;
  virtual std::any materialize(std::size_t row) const = 0;
  virtual std::unique_ptr<ColumnStorage>
  select(const std::vector<std::size_t> &rows) const = 0;
  virtual std::size_t approximateMemoryBytes() const = 0;
};

// Body scans bind variables to immutable relation cells. Keeping a reference
// here avoids copying a std::any (and often its heap allocation) at every join
// step. Computed values, such as aggregate outputs, are owned by the slot
// instead.
class BindingSlot {
public:
  BindingSlot() = default;

  BindingSlot(const BindingSlot &other) { copyFrom(other); }
  BindingSlot &operator=(const BindingSlot &other) {
    if (this != &other)
      copyFrom(other);
    return *this;
  }

  BindingSlot(BindingSlot &&other) noexcept { moveFrom(std::move(other)); }
  BindingSlot &operator=(BindingSlot &&other) noexcept {
    if (this != &other)
      moveFrom(std::move(other));
    return *this;
  }

  BindingSlot &operator=(const std::any &value) {
    bindOwned(value);
    return *this;
  }

  BindingSlot &operator=(std::any &&value) {
    bindOwned(std::move(value));
    return *this;
  }

  explicit operator bool() const { return static_cast<bool>(value_); }
  const ValueRef &operator*() const { return value_; }

  template <typename T> const T &get() const { return value_.get<T>(); }

  std::any materialize() const { return value_.materialize(); }

  void bindReference(const std::any &value) {
    owned_.reset();
    value_ = ValueRef::fromAny(value);
  }

  void bindReference(ValueRef value) {
    owned_.reset();
    value_ = value;
  }

  void bindOwned(std::any value) {
    owned_.emplace(std::move(value));
    value_ = ValueRef::fromAny(*owned_);
  }

  void reset() {
    owned_.reset();
    value_ = {};
  }

  bool ownsValue() const { return owned_.has_value(); }

private:
  void copyFrom(const BindingSlot &other) {
    if (other.owned_) {
      owned_ = other.owned_;
      value_ = ValueRef::fromAny(*owned_);
    } else {
      owned_.reset();
      value_ = other.value_;
    }
  }

  void moveFrom(BindingSlot &&other) {
    if (other.owned_) {
      owned_ = std::move(other.owned_);
      value_ = ValueRef::fromAny(*owned_);
    } else {
      owned_.reset();
      value_ = other.value_;
    }
    other.reset();
  }

  std::optional<std::any> owned_;
  ValueRef value_;
};

using Binding = std::vector<BindingSlot>;

enum class DependencyKind {
  Positive,
  Negative,
  Aggregate,
};

enum class RelationKind {
  Set,
  Lattice,
};

struct ColumnType {
  std::type_index type = typeid(void);
  std::string name;
  std::function<std::size_t(const std::any &)> hash;
  std::function<bool(const std::any &, const std::any &)> equal;
  std::function<std::size_t(ValueRef)> hash_value;
  std::function<bool(ValueRef, ValueRef)> equal_value;
  std::function<std::unique_ptr<ColumnStorage>()> make_storage;
  std::function<void(const std::any &)> validate;
  std::function<void(const std::any &)> validate_key;
};

struct ExprIR {
  std::type_index type = typeid(void);
  std::vector<VarId> referenced_vars;
  std::function<std::any(const Binding &)> evaluate;
  std::string debug_name;
};

struct TermIR {
  enum class Kind {
    Variable,
    Constant,
    Expression,
  };

  Kind kind = Kind::Constant;
  std::type_index type = typeid(void);
  VarId variable = 0;
  bool anonymous = false;
  std::any constant;
  ExprIR expression;
  std::string debug_name;
};

struct AtomIR {
  RelationId relation = 0;
  std::string relation_name;
  std::vector<TermIR> args;
};

struct FilterIR {
  ExprIR predicate;
};

struct NegAtomIR {
  AtomIR atom;
};

// User reducers are serial by default.  Parallel evaluation is enabled only
// when callers explicitly attest that partitioned add/merge is valid and that
// execution order cannot affect the observable result.
struct ReducerProperties {
  bool associative = false;
  bool commutative = false;
  bool deterministic = false;
  bool parallel_safe = false;

  static constexpr ReducerProperties parallel() {
    return {true, true, true, true};
  }

  constexpr bool canRunInParallel() const {
    return associative && commutative && deterministic && parallel_safe;
  }
};

struct ReducerIR {
  std::function<std::any()> make_state;
  std::function<void(std::any &, const std::any &)> add;
  std::function<void(std::any &, const std::any &)> merge;
  std::function<std::vector<std::any>(std::any &)> finish;
  ReducerProperties properties;
};

using AggregateConsumer = std::function<void(const std::any &)>;
using AggregateForEach = std::function<void(const AggregateConsumer &consumer)>;

struct AggregateIR {
  VarId output_var = 0;
  std::type_index output_type = typeid(void);
  AtomIR source;
  ExprIR projection;
  std::string name;
  std::function<std::vector<std::any>(const AggregateForEach &)> evaluate;
  std::optional<ReducerIR> reducer;
};

using BodyItemIR = std::variant<AtomIR, FilterIR, NegAtomIR, AggregateIR>;

struct RuleIR {
  AtomIR head;
  std::vector<BodyItemIR> body;
};

struct RelationIR {
  RelationId id = 0;
  std::string name;
  std::vector<ColumnType> columns;
  RelationKind kind = RelationKind::Set;
  std::function<bool(std::any &, const std::any &)> lattice_join;
};

} // namespace lotus::datalog
