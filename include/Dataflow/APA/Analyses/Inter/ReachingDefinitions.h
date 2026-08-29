#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTER_REACHINGDEFINITIONS_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTER_REACHINGDEFINITIONS_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachingDefinitionsDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimReachingDefinitionsCallStringLength = 2;

using InterReachingDefinitionsResult =
    InterDataFlowResultT<kDefaultInterElimReachingDefinitionsCallStringLength,
                         ReachingDefinitionsFact, llvm::Instruction *>;

InterReachingDefinitionsResult runInterElimReachingDefinitions(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::MemorySSA *MSSA = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr);

InterReachingDefinitionsResult runInterSummaryElimReachingDefinitions(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::MemorySSA *MSSA = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTER_REACHINGDEFINITIONS_H_
