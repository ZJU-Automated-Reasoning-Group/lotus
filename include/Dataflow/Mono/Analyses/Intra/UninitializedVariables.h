#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_UNINITIALIZEDVARIABLES_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_UNINITIALIZEDVARIABLES_H_

#include "Dataflow/Mono/Domains/UninitializedVariablesDomain.h"
#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/Mono/Support/MonoDebug.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

// Forward uninitialized variables analysis (intraprocedural).
std::unique_ptr<DataFlowResult>
runIntraMonoUninitVariables(llvm::Function *F,
                            const DebugConfig &DebugCfg = DebugConfig{});

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_UNINITIALIZEDVARIABLES_H_
