#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_CONSTANTPROPAGATION_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_CONSTANTPROPAGATION_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueLattice.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Adapters/LLVM/ForwardProblem.h"

#include <cstdint>
#include <unordered_map>

namespace elimination {

using ConstantPropagationValue = llvm::ValueLatticeElement;

using ConstantPropagationMap =
    std::unordered_map<const llvm::Value *, ConstantPropagationValue>;

using ConstantPropagationResult =
    DataFlowResultT<llvm::Instruction *, ConstantPropagationMap,
                    llvm::Instruction *>;

ConstantPropagationResult
runIntraElimConstantPropagation(llvm::Function *F,
                                EliminationOptions Opts = {});

ConstantPropagationResult
runIntraElimConstantPropagation(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts = {});

ConstantPropagationResult runIntraElimConstantPropagation(
    llvm::Function *F, llvm::AAResults *AA, llvm::AssumptionCache *AC,
    llvm::DominatorTree *DT, llvm::TargetLibraryInfo *TLI,
    EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_CONSTANTPROPAGATION_H_
