#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_INTRAUNINITVARIABLES_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_INTRAUNINITVARIABLES_H_

#include "Dataflow/Mono/Support/Result.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

// Forward uninitialized variables analysis (intraprocedural).
std::unique_ptr<DataFlowResult> runIntraMonoUninitVariables(llvm::Function *F);

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_INTRAUNINITVARIABLES_H_
