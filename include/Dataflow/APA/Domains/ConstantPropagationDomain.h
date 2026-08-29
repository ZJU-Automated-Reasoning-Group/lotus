#pragma once

#include "llvm/Analysis/ValueLattice.h"

#include <unordered_map>

namespace llvm {
class Value;
} // namespace llvm

namespace elimination {

using ConstantPropagationValue = llvm::ValueLatticeElement;
using ConstantPropagationMap =
    std::unordered_map<const llvm::Value *, ConstantPropagationValue>;

struct ConstantPropagationDomain {
  using value_type = ConstantPropagationMap;

  static value_type meet(const value_type &Lhs, const value_type &Rhs) {
    value_type Out = Lhs;
    for (const auto &Entry : Rhs) {
      auto It = Out.find(Entry.first);
      if (It == Out.end()) {
        ConstantPropagationValue Value;
        Value.mergeIn(Entry.second);
        Out.insert({Entry.first, Value});
      } else {
        It->second.mergeIn(Entry.second);
      }
    }
    return Out;
  }

  static bool equal(const value_type &Lhs, const value_type &Rhs) {
    for (const auto &Entry : Lhs) {
      auto It = Rhs.find(Entry.first);
      if (It == Rhs.end()) {
        if (!Entry.second.isUnknown())
          return false;
        continue;
      }
      if (!valueEqual(Entry.second, It->second))
        return false;
    }
    for (const auto &Entry : Rhs) {
      auto It = Lhs.find(Entry.first);
      if (It == Lhs.end() && !Entry.second.isUnknown())
        return false;
    }
    return true;
  }

  static value_type meetIdentity() { return {}; }

private:
  static bool valueEqual(const ConstantPropagationValue &Lhs,
                         const ConstantPropagationValue &Rhs) {
    if (Lhs.isUnknown() || Rhs.isUnknown())
      return Lhs.isUnknown() && Rhs.isUnknown();
    if (Lhs.isUndef() || Rhs.isUndef())
      return Lhs.isUndef() && Rhs.isUndef();
    if (Lhs.isOverdefined() || Rhs.isOverdefined())
      return Lhs.isOverdefined() && Rhs.isOverdefined();
    if (Lhs.isConstant() || Rhs.isConstant())
      return Lhs.isConstant() && Rhs.isConstant() &&
             Lhs.getConstant() == Rhs.getConstant();
    if (Lhs.isNotConstant() || Rhs.isNotConstant())
      return Lhs.isNotConstant() && Rhs.isNotConstant() &&
             Lhs.getNotConstant() == Rhs.getNotConstant();
    if (Lhs.isConstantRange() || Rhs.isConstantRange())
      return Lhs.isConstantRange(true) && Rhs.isConstantRange(true) &&
             Lhs.isConstantRangeIncludingUndef() ==
                 Rhs.isConstantRangeIncludingUndef() &&
             Lhs.getConstantRange(true) == Rhs.getConstantRange(true);
    return false;
  }
};

} // namespace elimination
