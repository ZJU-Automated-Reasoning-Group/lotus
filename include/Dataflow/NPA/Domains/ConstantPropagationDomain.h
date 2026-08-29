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

enum class ConstantPropagationTag {
  Top,
  Const,
};

struct ConstantPropagationValue {
  ConstantPropagationTag tag = ConstantPropagationTag::Top;
  llvm::APInt constant = llvm::APInt(1, 0);

  bool isConstant() const { return tag == ConstantPropagationTag::Const; }

  bool operator==(const ConstantPropagationValue &Other) const {
    return tag == Other.tag &&
           (!isConstant() ||
            (constant.getBitWidth() == Other.constant.getBitWidth() &&
             constant.eq(Other.constant)));
  }
};

struct ConstantPropagationState {
  bool reachable = false;
  std::unordered_map<const llvm::Value *, ConstantPropagationValue> values;

  bool operator==(const ConstantPropagationState &Other) const {
    return reachable == Other.reachable && values == Other.values;
  }
};

struct ConstantPropagationOp {
  enum class Kind {
    AssignConst,
    Copy,
    Cast,
    Binary,
    Compare,
    AssumeNotCases,
    Phi,
    Select,
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
  llvm::APInt constant = llvm::APInt(1, 0);
  std::vector<const llvm::Value *> inputs;

  bool operator<(const ConstantPropagationOp &Other) const;
  bool operator==(const ConstantPropagationOp &Other) const;
  bool summaryCanBeOverwritten() const;
  bool summaryCanOverwritePrevious() const;
};

using ConstantPropagationSummary = TransformerSummary<ConstantPropagationOp>;

} // namespace npa
