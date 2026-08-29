#pragma once

#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <set>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Value.h>

namespace mono {

struct AvailableExpression {
  unsigned Opcode;
  llvm::SmallVector<llvm::Value *, 4> Operands;

  AvailableExpression(unsigned Opcode, llvm::ArrayRef<llvm::Value *> Ops)
      : Opcode(Opcode), Operands(Ops.begin(), Ops.end()) {}

  bool operator==(const AvailableExpression &Other) const {
    return Opcode == Other.Opcode && Operands == Other.Operands;
  }

  bool operator<(const AvailableExpression &Other) const {
    if (Opcode != Other.Opcode)
      return Opcode < Other.Opcode;
    return Operands < Other.Operands;
  }

  bool usesValue(llvm::Value *Value) const {
    return llvm::find(Operands, Value) != Operands.end();
  }
};

struct AvailableExpressionsDomain
    : IntersectionDomain<std::set<AvailableExpression>> {
  using IntersectionDomain::IntersectionDomain;
};

using AvailableExpressionsAnalysisTypes =
    LLVMMonoAnalysisTypes<AvailableExpressionsDomain::value_type,
                          AvailableExpressionsDomain>;

} // namespace mono
