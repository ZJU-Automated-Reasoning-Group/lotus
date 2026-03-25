#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTER_INTERFULLCONSTANTPROPAGATION_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTER_INTERFULLCONSTANTPROPAGATION_H_

#include "Dataflow/Mono/Analyses/Intra/IntraFullConstantPropagation.h"
#include "Dataflow/Mono/Core/CallStringSolver.h"

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

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTER_INTERFULLCONSTANTPROPAGATION_H_
