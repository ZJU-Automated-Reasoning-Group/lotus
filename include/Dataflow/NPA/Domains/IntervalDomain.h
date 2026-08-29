#pragma once

#include "Dataflow/NPA/Domains/TransformerSummary.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/APInt.h>

namespace llvm {
class Value;
} // namespace llvm

namespace npa {

enum class IntervalOrdering {
  Signed,
  Unsigned,
};

struct Interval {
  bool bottom = false;
  bool hasLower = false;
  bool hasUpper = false;
  IntervalOrdering ordering = IntervalOrdering::Signed;
  llvm::APInt lower = llvm::APInt(1, 0);
  llvm::APInt upper = llvm::APInt(1, 0);

  static Interval top(unsigned BitWidth = 1,
                      IntervalOrdering Ordering = IntervalOrdering::Signed) {
    Interval Out;
    Out.ordering = Ordering;
    Out.lower = llvm::APInt(BitWidth, 0);
    Out.upper = llvm::APInt(BitWidth, 0);
    return Out;
  }

  static Interval point(const llvm::APInt &Value,
                        IntervalOrdering Ordering = IntervalOrdering::Signed) {
    Interval Out;
    Out.hasLower = true;
    Out.hasUpper = true;
    Out.ordering = Ordering;
    Out.lower = Value;
    Out.upper = Value;
    return Out;
  }

  bool isExact() const {
    return hasLower && hasUpper && lower.getBitWidth() == upper.getBitWidth() &&
           lower.eq(upper);
  }

  bool operator==(const Interval &Other) const {
    return bottom == Other.bottom && hasLower == Other.hasLower &&
           hasUpper == Other.hasUpper && ordering == Other.ordering &&
           (!hasLower || (lower.getBitWidth() == Other.lower.getBitWidth() &&
                          lower.eq(Other.lower))) &&
           (!hasUpper || (upper.getBitWidth() == Other.upper.getBitWidth() &&
                          upper.eq(Other.upper)));
  }
};

struct IntervalState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, Interval> values;

  bool operator==(const IntervalState &Other) const {
    return reachable == Other.reachable && values == Other.values;
  }
};

struct IntervalOp {
  enum class Kind {
    AssignConst,
    Copy,
    Cast,
    Binary,
    Compare,
    AssumeNotCases,
    Select,
    Phi,
    Forget,
  };

  Kind kind = Kind::Forget;
  const llvm::Value *dest = nullptr;
  const llvm::Value *lhs = nullptr;
  const llvm::Value *rhs = nullptr;
  const llvm::Value *cond = nullptr;
  unsigned opcode = 0;
  unsigned bitWidth = 0;
  unsigned sourceBitWidth = 0;
  IntervalOrdering ordering = IntervalOrdering::Signed;
  llvm::APInt constant = llvm::APInt(1, 0);
  std::vector<const llvm::Value *> inputs;

  bool operator<(const IntervalOp &Other) const;
  bool operator==(const IntervalOp &Other) const;
  bool summaryCanBeOverwritten() const;
  bool summaryCanOverwritePrevious() const;
};

using IntervalSummary = TransformerSummary<IntervalOp>;

} // namespace npa
