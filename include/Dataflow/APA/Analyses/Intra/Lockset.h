#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_LOCKSET_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_LOCKSET_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"
#include "Dataflow/APA/Domains/LocksetDomain.h"

namespace elimination {

using LocksetResult =
    DataFlowResultT<llvm::Instruction *, LocksetFact, llvm::Instruction *>;

LocksetResult runIntraElimLockset(llvm::Function *F,
                                  EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_LOCKSET_H_
