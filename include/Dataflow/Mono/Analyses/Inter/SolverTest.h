#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTER_SOLVERTEST_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTER_SOLVERTEST_H_

#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"
#include "Dataflow/Mono/LLVM/AnalysisTypes.h"

#include <memory>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace mono {

struct InterMonoSolverTestDomain
    : LLVMMonoAnalysisTypes<SetContainer<llvm::Value *>> {};

constexpr unsigned kDefaultInterMonoSolverTestCallStringLength = 2;
using InterMonoSolverTestResult = dataflow::ContextSensitiveDataFlowResult<
    kDefaultInterMonoSolverTestCallStringLength, SetContainer<llvm::Value *>>;

struct InterMonoSolverTestAnalysisResult {
  std::unique_ptr<InterMonoSolverTestResult> Results;
};

InterMonoSolverTestAnalysisResult runInterMonoSolverTest(llvm::Function *Entry);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTER_SOLVERTEST_H_
