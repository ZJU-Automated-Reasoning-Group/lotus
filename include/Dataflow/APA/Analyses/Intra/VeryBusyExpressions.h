#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_VERYBUSYEXPRESSIONS_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_VERYBUSYEXPRESSIONS_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/VeryBusyExpressionsDomain.h"

namespace elimination {

using VeryBusyExpressionsResult =
    DataFlowResultT<llvm::Instruction *, VeryBusyExpressionsFact,
                    llvm::Instruction *>;

// Backward, must-style very-busy expressions. For multi-return functions, the
// implementation solves one reverse problem per return and intersects facts.
VeryBusyExpressionsResult
runIntraElimVeryBusyExpressions(llvm::Function *F,
                                EliminationOptions Opts = {});

VeryBusyExpressionsResult
runIntraElimVeryBusyExpressions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts = {});

VeryBusyExpressionsResult runIntraElimVeryBusyExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, EliminationOptions Opts = {});

VeryBusyExpressionsResult runIntraElimVeryBusyExpressions(
    llvm::Function *F, llvm::AAResults *AA, llvm::DominatorTree *DT,
    llvm::TargetLibraryInfo *TLI, llvm::MemorySSA *MSSA,
    EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_VERYBUSYEXPRESSIONS_H_
