#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_UNINITIALIZEDVARIABLES_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_UNINITIALIZEDVARIABLES_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/UninitializedVariablesDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using UninitVariablesResult =
    DataFlowResultT<llvm::Instruction *, UninitVariablesFact,
                    llvm::Instruction *>;

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  EliminationOptions Opts = {});

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  llvm::AAResults *AA,
                                                  EliminationOptions Opts = {});

UninitVariablesResult runIntraElimUninitVariables(llvm::Function *F,
                                                  llvm::AAResults *AA,
                                                  llvm::AssumptionCache *AC,
                                                  llvm::DominatorTree *DT,
                                                  EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_UNINITIALIZEDVARIABLES_H_
