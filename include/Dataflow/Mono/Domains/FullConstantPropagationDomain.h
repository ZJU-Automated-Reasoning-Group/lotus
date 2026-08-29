#pragma once

#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <cstdint>
#include <unordered_map>

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

enum class FullConstantTag {
  Bottom,
  Const,
  Top,
};

struct FullConstantValue {
  FullConstantTag Tag;
  int64_t ConstValue;

  static FullConstantValue bottom() { return {FullConstantTag::Bottom, 0}; }
  static FullConstantValue top() { return {FullConstantTag::Top, 0}; }
  static FullConstantValue constant(int64_t Value) {
    return {FullConstantTag::Const, Value};
  }

  bool operator==(const FullConstantValue &Other) const {
    return Tag == Other.Tag && ConstValue == Other.ConstValue;
  }
};

struct FullConstantPropagationState {
  bool Unreachable = true;
  std::unordered_map<const llvm::Value *, FullConstantValue> Values;

  bool empty() const { return Unreachable; }
};

class FullConstantPropagationDomain {
public:
  using value_type = FullConstantPropagationState;
  static constexpr bool is_legacy = false;

  value_type bottom() const { return {}; }

  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    if (Lhs.Unreachable)
      return Rhs;
    if (Rhs.Unreachable)
      return Lhs;

    value_type Out;
    Out.Unreachable = false;
    Out.Values = Lhs.Values;
    for (const auto &Entry : Rhs.Values) {
      auto It = Out.Values.find(Entry.first);
      if (It == Out.Values.end())
        Out.Values[Entry.first] =
            joinValues(FullConstantValue::top(), Entry.second);
      else
        It->second = joinValues(It->second, Entry.second);
    }
    for (const auto &Entry : Lhs.Values) {
      if (Rhs.Values.find(Entry.first) == Rhs.Values.end())
        Out.Values[Entry.first] =
            joinValues(Entry.second, FullConstantValue::top());
    }
    return Out;
  }

  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    if (Lhs.Unreachable != Rhs.Unreachable)
      return false;
    if (Lhs.Unreachable)
      return true;
    for (const auto &Entry : Lhs.Values) {
      if (!(Entry.second == lookupOrTop(Rhs, Entry.first)))
        return false;
    }
    for (const auto &Entry : Rhs.Values) {
      if (!(lookupOrTop(Lhs, Entry.first) == Entry.second))
        return false;
    }
    return true;
  }

  value_type widen(const value_type &, const value_type &NewValue) const {
    return NewValue;
  }

private:
  static FullConstantValue joinValues(const FullConstantValue &Lhs,
                                      const FullConstantValue &Rhs) {
    if (Lhs.Tag == FullConstantTag::Bottom)
      return Rhs;
    if (Rhs.Tag == FullConstantTag::Bottom)
      return Lhs;
    if (Lhs.Tag == FullConstantTag::Const &&
        Rhs.Tag == FullConstantTag::Const && Lhs.ConstValue == Rhs.ConstValue)
      return Lhs;
    return FullConstantValue::top();
  }
  static FullConstantValue lookupOrTop(const value_type &State,
                                       const llvm::Value *Value) {
    auto It = State.Values.find(Value);
    return It == State.Values.end() ? FullConstantValue::top() : It->second;
  }
};

using FullConstantPropagationAnalysisTypes =
    LLVMMonoAnalysisTypes<FullConstantPropagationDomain::value_type,
                          FullConstantPropagationDomain>;

} // namespace mono
