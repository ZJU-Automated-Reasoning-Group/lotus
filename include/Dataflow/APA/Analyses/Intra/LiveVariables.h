#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_LIVEVARIABLES_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_LIVEVARIABLES_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/LiveVariablesDomain.h"

namespace elimination {

using LiveVariablesResult =
    DataFlowResultT<llvm::Instruction *, LiveVariablesFact,
                    llvm::Instruction *>;

// Backward, may-style liveness. For multi-return functions, the implementation
// solves one reverse problem per return and unions per-node facts.
LiveVariablesResult runIntraElimLiveVariables(llvm::Function *F,
                                              EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_LIVEVARIABLES_H_
