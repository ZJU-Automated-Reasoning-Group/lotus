#ifndef DATAFLOW_APA_CLIENTS_LLVM_INTRA_SIGN_H_
#define DATAFLOW_APA_CLIENTS_LLVM_INTRA_SIGN_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include "Dataflow/APA/APA.h"
#include "Dataflow/APA/LLVM/ForwardProblem.h"
#include "Dataflow/APA/Domains/SignDomain.h"

namespace elimination {

using SignAnalysisResult =
    DataFlowResultT<llvm::Instruction *, SignMap, llvm::Instruction *>;

SignAnalysisResult runIntraElimSignAnalysis(llvm::Function *F,
                                            EliminationOptions Opts = {});

} // namespace elimination

#endif // DATAFLOW_APA_CLIENTS_LLVM_INTRA_SIGN_H_
