#pragma once

#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <cstdint>
#include <unordered_map>

namespace llvm {
class Value;
} // namespace llvm

namespace mono {

enum class ConstantPropagationTag {
  Top,
  Const,
  Bottom,
};

struct ConstantPropagationValue {
  ConstantPropagationTag Tag = ConstantPropagationTag::Top;
  int64_t ConstValue = 0;

  bool operator==(const ConstantPropagationValue &Other) const {
    return Tag == Other.Tag && ConstValue == Other.ConstValue;
  }
};

using ConstantPropagationMap =
    std::unordered_map<const llvm::Value *, ConstantPropagationValue>;

class ConstantPropagationDomain {
public:
  using value_type = ConstantPropagationMap;
  static constexpr bool is_legacy = false;

  value_type bottom() const { return {{nullptr, makeBottomValue()}}; }
  bool isBottom(const value_type &Value) const {
    return isMergeIdentity(Value);
  }

  value_type join(const value_type &Lhs, const value_type &Rhs) const {
    if (isMergeIdentity(Lhs))
      return Rhs;
    if (isMergeIdentity(Rhs))
      return Lhs;

    value_type Out;
    for (const auto &Entry : Lhs) {
      if (Entry.first == nullptr)
        continue;
      Out[Entry.first] =
          joinValues(Entry.second, lookupOrTop(Rhs, Entry.first));
    }
    for (const auto &Entry : Rhs) {
      if (Entry.first == nullptr)
        continue;
      if (Lhs.find(Entry.first) == Lhs.end())
        Out[Entry.first] = joinValues(makeTopValue(), Entry.second);
    }
    return Out;
  }

  bool equal(const value_type &Lhs, const value_type &Rhs) const {
    if (isMergeIdentity(Lhs) || isMergeIdentity(Rhs))
      return isMergeIdentity(Lhs) && isMergeIdentity(Rhs);
    for (const auto &Entry : Lhs) {
      if (Entry.first == nullptr)
        continue;
      if (!(Entry.second == lookupOrTop(Rhs, Entry.first)))
        return false;
    }
    for (const auto &Entry : Rhs) {
      if (Entry.first == nullptr)
        continue;
      if (!(lookupOrTop(Lhs, Entry.first) == Entry.second))
        return false;
    }
    return true;
  }

  value_type widen(const value_type &, const value_type &NewValue) const {
    return NewValue;
  }

private:
  static ConstantPropagationValue makeTopValue() {
    return {ConstantPropagationTag::Top, 0};
  }
  static ConstantPropagationValue makeBottomValue() {
    return {ConstantPropagationTag::Bottom, 0};
  }
  static ConstantPropagationValue lookupOrTop(const value_type &Map,
                                              const llvm::Value *Value) {
    auto It = Map.find(Value);
    return It == Map.end() ? makeTopValue() : It->second;
  }
  static ConstantPropagationValue
  joinValues(const ConstantPropagationValue &Lhs,
             const ConstantPropagationValue &Rhs) {
    if (Lhs == Rhs)
      return Lhs;
    if (Lhs.Tag == ConstantPropagationTag::Bottom)
      return Rhs;
    if (Rhs.Tag == ConstantPropagationTag::Bottom)
      return Lhs;
    return makeTopValue();
  }
  static bool isMergeIdentity(const value_type &Map) {
    return Map.size() == 1 && Map.count(nullptr) == 1u;
  }
};

using ConstantPropagationAnalysisTypes =
    LLVMMonoAnalysisTypes<ConstantPropagationDomain::value_type,
                          ConstantPropagationDomain>;

} // namespace mono
