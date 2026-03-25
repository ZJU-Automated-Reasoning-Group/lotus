/// \file Value.h
/// \brief FiTx value representation with field-sensitive tracking.
///
/// This file defines the core value abstraction for the FiTx typestate analysis
/// framework. Values represent program objects that can be tracked through
/// their lifecycle states (e.g., allocated, freed, locked).
///
/// **Field Sensitivity** (paper §3):
/// - Tracks static field offsets for struct fields and constant array indices
/// - Does NOT compute runtime field offsets (keeps analysis tractable)
/// - Each Value has: (base LLVM value, field access chain, array element index)
///
/// **Value Hierarchy**:
/// ```
/// Value (base)
///   ├── ConstValue    - Integer constants (e.g., return codes)
///   ├── NullValue     - Null pointer constants
///   └── Argument      - Function arguments (for inter-procedural analysis)
/// ```
///
/// **Related Classes**:
/// - ValueCollection: Set of values with set operations
/// - AliasValues: May-alias map (value -> set of aliased values)
/// - ManagedValues: Singleton registry of all created values
///
/// \see Suzuki et al., "Balancing Analysis Time and Bug Detection", USENIX ATC
/// 2024
#pragma once
#include "llvm/IR/Constants.h"
#include "llvm/IR/Value.h"

#include <map>
#include <queue>
#include <set>
#include <vector>

namespace fitx {

class Instruction;
class Value {
public:
  constexpr static int kNonFieldVariable = -1;
  constexpr static int kNonArrayElement = -2;
  constexpr static int kArbitaryArrayElement = -1;

  /// Static field offset: struct field or constant array index (paper §3).
  struct Fields {
    Fields(llvm::Type *type, long field = kNonFieldVariable)
        : type(type), field(field) {}
    llvm::Type *type;
    long field;
    bool operator==(const Fields &value) const {
      return type == value.type && field == value.field;
    }

    bool operator!=(const Fields &value) const {
      if (type == value.type)
        return field != value.field;
      return type != value.type;
    }
    llvm::Type *ElementType() const {
      return type->isPointerTy() ? type->getPointerElementType() : type;
    }

    llvm::Type *FieldType() const {
      llvm::Type *element = ElementType();
      if (field == kNonFieldVariable || !element->isStructTy() ||
          element->getStructNumElements() < field)
        return nullptr;
      return element->getStructElementType(field);
    }
  };
  using FieldList = std::vector<Fields>;

  // factory
  static std::shared_ptr<Value> CreateFromDefinition(llvm::Value *value);
  static std::shared_ptr<Value> CreateAppend(std::shared_ptr<Value> src,
                                             std::shared_ptr<Value> target);
  static std::shared_ptr<Value>
  CreateManagedValue(std::shared_ptr<fitx::Instruction> value);

  Value(llvm::Value *value, std::vector<Fields> fields, long array_element_num);
  Value(std::shared_ptr<fitx::Value> value, std::vector<Fields> fields,
        long array_element_num);
  Value(const Value &value);

  Value(unsigned value_type = 0)
      : value_(nullptr), array_element_num_(kNonArrayElement),
        fields_(std::vector<Fields>()), is_global_var_(false),
        is_return_value_(false), value_type_(value_type) {};

  Value(llvm::Value *value)
      : value_(value), array_element_num_(kNonArrayElement),
        fields_(std::vector<Fields>()),
        is_global_var_(llvm::isa<llvm::GlobalValue>(value)),
        value_type_(value->getValueID()) {};

  Value(std::shared_ptr<Value> value);

  bool operator<(const Value &V) const;
  bool operator==(const Value &V) const;
  bool operator==(const llvm::Value *V) const;
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &ostream,
                                       const fitx::Value &value);

  llvm::Value &getLLVMValue_() const;
  llvm::Type &getLLVMType_() const;

  bool isArgument();
  bool isGlobalVar();
  bool isReturnValue();
  void setReturnValue(bool is_return_value);

  long ArrayElementNum();
  bool isArbitaryArrayElement() {
    return array_element_num_ == kArbitaryArrayElement;
  }

  long Field() const;
  const std::vector<Fields> &GetFields() const;

  // TODO: This function returns the type of the value defined in the LLVM.
  // The function name is left as getValueID to be compatible with LLVM,
  // but should be renamed tr o getValueType(), as it is very confusing
  // with the actual ID of this value.
  const unsigned getValueID() const { return value_type_; };

  const bool isRoot() const { return fields_.empty(); }

  void addUser(std::shared_ptr<fitx::Value> user) {
    users_.push_back(user);
  }

  std::vector<std::weak_ptr<fitx::Value>> Users() { return users_; }

  void setManagedId(size_t id) { managed_id_ = id; }
  size_t ManagedId() { return managed_id_; }

private:
  size_t managed_id_;

  llvm::Value *value_;

  unsigned value_type_;

  long array_element_num_;
  bool is_global_var_;
  bool is_return_value_;
  std::vector<Fields> fields_;
  std::vector<std::weak_ptr<fitx::Value>> users_;
};

class ConstValue : public Value {
public:
  ConstValue(llvm::ConstantInt *value);
  ConstValue(int64_t const_value);
  ConstValue(std::shared_ptr<ConstValue> value);

  int64_t getConstValue() { return const_value_; }

  static bool classof(const fitx::Value *value) {
    // Methods for support type inquiry through isa, cast, and dyn_cast:
    return value->getValueID() == llvm::Value::ConstantIntVal;
  }

private:
  std::int64_t const_value_;
};

class NullValue : public Value {
public:
  NullValue(llvm::ConstantPointerNull *value);
  NullValue(std::shared_ptr<NullValue> value);

  static bool classof(const fitx::Value *value) {
    // Methods for support type inquiry through isa, cast, and dyn_cast:
    return value->getValueID() == llvm::Value::ConstantPointerNullVal;
  }
};

class Argument : public Value {
public:
  Argument(llvm::Argument *argument, std::vector<Value::Fields> fields,
           long array_element_num);

  Argument(std::shared_ptr<Argument> argument,
           std::vector<Value::Fields> fields, long array_element_num);

  uint64_t ArgNum() const { return arg_num_; }

  static bool classof(const fitx::Value *value) {
    // Methods for support type inquiry through isa, cast, and dyn_cast:
    return value->getValueID() == llvm::Value::ArgumentVal;
  }

private:
  uint64_t arg_num_;
};

/// Set of framework values with set operations and SSA/field "related" lookup.
/// getRelatedValues() returns values that share the same base LLVM value and
/// compatible field path (same or more specific); used to propagate typestate
/// to all SSA/field variants of a pointer (paper §3).
class ValueCollection {
public:
  ValueCollection() = default;

  // factories
  static ValueCollection
  createFromIntersection(const std::vector<ValueCollection> &collections);
  static ValueCollection
  createFromUnion(const std::vector<ValueCollection> &collections);

  const std::set<std::shared_ptr<Value>> &Values() const { return values_; };

  bool exists(fitx::Value value);
  bool exists(std::shared_ptr<Value> val);

  bool add(std::shared_ptr<Value> val);

  void add(const ValueCollection &collection);
  void remove(std::shared_ptr<Value> val);
  void clear();

  /// Values with same base LLVM value and compatible field path (SSA/field
  /// "related"); not alias—used to broaden store/branch transition targets.
  std::set<std::shared_ptr<Value>>
  getRelatedValues(std::shared_ptr<Value> value) const;

  std::set<std::shared_ptr<Value>>
  getParentValues(std::shared_ptr<Value> value) const;

  size_t size() { return values_.size(); }

private:
  std::set<std::shared_ptr<Value>> values_;
};

/// May-alias sets for typestate propagation (paper §3: intra-procedural only).
/// Recorded on stores (ptr = value_operand); used when applying store/alias
/// transitions so all possibly-aliased values get updated. Not merged across
/// CFG predecessors (inter-block merge is commented out in Function.cpp).
class AliasValues {
public:
  AliasValues();

  void addAlias(std::shared_ptr<fitx::Value> src,
                std::shared_ptr<fitx::Value> target);

  void addAlias(AliasValues &collection);

  /// Returns the set of values that may alias \p value, or nullptr if none.
  const ValueCollection *getAliasInfo(std::shared_ptr<fitx::Value> value);

  const std::vector<std::shared_ptr<fitx::Value>> &UpdatedValues() {
    return updated_values_;
  };

  const std::map<std::shared_ptr<fitx::Value>, ValueCollection> &
  AliasInfo() {
    return alias_info_;
  };

  size_t Size() { return alias_size_; }

private:
  std::map<std::shared_ptr<fitx::Value>, ValueCollection> alias_info_;

  std::vector<std::shared_ptr<fitx::Value>> updated_values_;
  size_t alias_size_;
};

/// Singleton registry of all framework values (for stable IDs / serialization).
class ManagedValues {
public:
  // Bug fix: return by reference so addValue() mutations are not silently
  // discarded on a temporary copy. Previously returned by value.
  static ManagedValues &GetInstance();
  constexpr static size_t kReserveSize = 10000;

  size_t Size() { return managed_values_.size(); }

  void addValue(std::shared_ptr<fitx::Value> value) {
    value->setManagedId(Size());
    return managed_values_.push_back(value);
  }

  std::shared_ptr<fitx::Value> getValueFromID(size_t id);

private:
  ManagedValues();
  std::vector<std::shared_ptr<fitx::Value>> managed_values_;
};

}; // namespace fitx
