#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_SOLVERTEST_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_SOLVERTEST_H_

#include "Dataflow/Mono/LLVM/AnalysisTypes.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"
#include "Dataflow/Mono/Support/Result.h"

#include <memory>
#include <set>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct IntraMonoSolverTestAnalysisTypes
    : LLVMMonoAnalysisTypes<std::set<llvm::Value *>> {};

std::unique_ptr<DataFlowResult> runIntraMonoSolverTest(llvm::Function *F);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_SOLVERTEST_H_
