#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTER_UNINITIALIZEDVARIABLES_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTER_UNINITIALIZEDVARIABLES_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/UninitializedVariablesDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimUninitVariablesCallStringLength = 2;

using InterUninitVariablesResult =
    InterDataFlowResultT<kDefaultInterElimUninitVariablesCallStringLength,
                         UninitVariablesFact, llvm::Instruction *>;

InterUninitVariablesResult runInterElimUninitVariables(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr);

InterUninitVariablesResult runInterSummaryElimUninitVariables(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTER_UNINITIALIZEDVARIABLES_H_
