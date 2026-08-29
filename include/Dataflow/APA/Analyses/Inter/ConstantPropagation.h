#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTER_CONSTANTPROPAGATION_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTER_CONSTANTPROPAGATION_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ConstantPropagationDomain.h"

namespace elimination {

constexpr unsigned kDefaultInterElimConstantPropagationCallStringLength = 2;

using InterConstantPropagationResult =
    InterDataFlowResultT<kDefaultInterElimConstantPropagationCallStringLength,
                         ConstantPropagationMap, llvm::Instruction *>;

InterConstantPropagationResult runInterElimConstantPropagation(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
    llvm::TargetLibraryInfo *TLI = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr);

InterConstantPropagationResult runInterSummaryElimConstantPropagation(
    llvm::Function *Entry, llvm::AAResults *AA = nullptr,
    llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
    llvm::TargetLibraryInfo *TLI = nullptr,
    const dataflow::controlflow::InterCFG *ICF = nullptr,
    PathSummaryEquationOptions Options = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTER_CONSTANTPROPAGATION_H_
