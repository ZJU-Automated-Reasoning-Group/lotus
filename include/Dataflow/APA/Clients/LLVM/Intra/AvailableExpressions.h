#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_AVAILABLEEXPRESSIONS_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_AVAILABLEEXPRESSIONS_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Adapters/LLVM/ForwardProblem.h"
#include "Dataflow/APA/Clients/LLVM/ExpressionKey.h"

#include <set>

namespace elimination {

using AvailableExpressionsFact = std::set<ExpressionKey>;
using AvailableExpressionsResult =
    DataFlowResultT<llvm::Instruction *, AvailableExpressionsFact,
                    llvm::Instruction *>;

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F,
                                 EliminationOptions Opts = {});

AvailableExpressionsResult
runIntraElimAvailableExpressions(llvm::Function *F, llvm::AAResults *AA,
                                 EliminationOptions Opts = {});

AvailableExpressionsResult runIntraElimAvailableExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, EliminationOptions Opts = {});

AvailableExpressionsResult runIntraElimAvailableExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, llvm::MemorySSA *MSSA,
    EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_AVAILABLEEXPRESSIONS_H_
