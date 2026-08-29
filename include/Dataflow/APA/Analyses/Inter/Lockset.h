#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTER_LOCKSET_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTER_LOCKSET_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/LocksetDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimLocksetCallStringLength = 2;

using InterLocksetResult =
    InterDataFlowResultT<kDefaultInterElimLocksetCallStringLength, LocksetFact,
                         llvm::Instruction *>;

InterLocksetResult
runInterElimLockset(llvm::Function *Entry,
                    const dataflow::controlflow::InterCFG *ICF = nullptr);

InterLocksetResult
runInterSummaryElimLockset(llvm::Function *Entry,
                           const dataflow::controlflow::InterCFG *ICF = nullptr,
                           PathSummaryEquationOptions Options = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTER_LOCKSET_H_
