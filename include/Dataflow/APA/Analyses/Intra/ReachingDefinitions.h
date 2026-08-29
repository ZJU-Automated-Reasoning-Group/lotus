#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHINGDEFINITIONS_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHINGDEFINITIONS_H_

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/Domains/ReachingDefinitionsDomain.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"

namespace elimination {

using ReachingDefinitionsResult =
    DataFlowResultT<llvm::Instruction *, ReachingDefinitionsFact,
                    llvm::Instruction *>;

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F,
                                EliminationOptions Opts = {});

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts = {});

ReachingDefinitionsResult
runIntraElimReachingDefinitions(llvm::Function *F, llvm::AAResults *AA,
                                llvm::MemorySSA *MSSA,
                                EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_REACHINGDEFINITIONS_H_
