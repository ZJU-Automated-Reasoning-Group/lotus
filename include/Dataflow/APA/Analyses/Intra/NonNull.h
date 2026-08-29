#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_NONNULL_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_NONNULL_H_

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/NonNullDomain.h"

namespace elimination {

// Edge-sensitive transfer for branch-conditioned facts (e.g., pointer != null
// on one successor only).
struct NonNullEdgeTransfer {
  llvm::Instruction *Src = nullptr;
  llvm::Instruction *Dst = nullptr;
};

using NonNullResult =
    DataFlowResultT<llvm::Instruction *, NonNullFact, NonNullEdgeTransfer>;

NonNullResult runIntraElimNonNull(llvm::Function *F,
                                  EliminationOptions Opts = {});

NonNullResult runIntraElimNonNull(llvm::Function *F, llvm::AssumptionCache *AC,
                                  llvm::DominatorTree *DT,
                                  EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_NONNULL_H_
