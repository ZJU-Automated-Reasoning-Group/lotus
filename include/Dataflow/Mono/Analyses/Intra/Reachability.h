#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_REACHABILITY_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_REACHABILITY_H_

#include "Dataflow/Mono/Domains/ReachabilityDomain.h"
#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/Mono/Support/MonoDebug.h"

#include <functional>
#include <memory>

namespace llvm {
class Function;
class Instruction;
} // namespace llvm

namespace mono {

// Compute forward reachability using backward dataflow analysis.
// This analysis determines which instructions can be executed from each program
// point.
std::unique_ptr<DataFlowResult>
runReachableAnalysis(llvm::Function *f,
                     const DebugConfig &DebugCfg = DebugConfig{});

std::unique_ptr<DataFlowResult>
runReachableAnalysis(llvm::Function *f,
                     const std::function<bool(llvm::Instruction *i)> &filter,
                     const DebugConfig &DebugCfg = DebugConfig{});

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_REACHABILITY_H_
