#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTER_FULLCONSTANTPROPAGATION_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTER_FULLCONSTANTPROPAGATION_H_

#include "Dataflow/Mono/Domains/FullConstantPropagationDomain.h"
#include "Dataflow/Mono/Solver/CallStringSolver.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

constexpr unsigned kDefaultFullConstantPropagationCallStringLength = 2;
using InterMonoFullConstantPropagationResult =
    dataflow::ContextSensitiveDataFlowResult<
        kDefaultFullConstantPropagationCallStringLength,
        FullConstantPropagationState>;

struct InterMonoFullConstantPropagationAnalysisResult {
  std::unique_ptr<InterMonoFullConstantPropagationResult> Results;
};

InterMonoFullConstantPropagationAnalysisResult
runInterMonoFullConstantPropagation(llvm::Function *Entry);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTER_FULLCONSTANTPROPAGATION_H_
