#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTER_REACHABILITY_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTER_REACHABILITY_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachabilityDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimReachabilityCallStringLength = 2;

using InterReachableResult =
    InterDataFlowResultT<kDefaultInterElimReachabilityCallStringLength,
                         ReachableFact, llvm::Instruction *>;

InterReachableResult
runInterElimReachable(llvm::Function *Entry,
                      const dataflow::controlflow::InterCFG *ICF = nullptr);

InterReachableResult runInterSummaryElimReachable(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTER_REACHABILITY_H_
