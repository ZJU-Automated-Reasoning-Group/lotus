#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHABILITY_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHABILITY_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachabilityDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using ReachableResult =
    DataFlowResultT<llvm::Instruction *, ReachableFact, llvm::Instruction *>;

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHABILITY_H_
