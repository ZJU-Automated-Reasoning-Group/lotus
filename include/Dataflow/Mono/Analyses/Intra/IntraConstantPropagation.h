#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_INTRACONSTANTPROPAGATION_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_INTRACONSTANTPROPAGATION_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

#include <cstdint>
#include <unordered_map>

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

struct ConstantPropagationDomain
    : LLVMMonoAnalysisDomain<ConstantPropagationMap> {};

using ConstantPropagationSolver = IntraMonoSolver<ConstantPropagationDomain>;

std::unordered_map<llvm::Instruction *, ConstantPropagationMap>
runIntraMonoConstantPropagation(llvm::Function *F);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_INTRACONSTANTPROPAGATION_H_
