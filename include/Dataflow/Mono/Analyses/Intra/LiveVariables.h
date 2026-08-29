#ifndef LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_LIVEVARIABLES_H_
#define LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_LIVEVARIABLES_H_

#include "Dataflow/Mono/Domains/LiveVariablesDomain.h"
#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/Mono/Support/MonoDebug.h"

#include <memory>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

/**
 * @brief Run SSA register liveness analysis on a function
 *
 * This is a backward dataflow analysis that computes which SSA values
 * (registers) are "live" (will be used in the future) at each program point.
 *
 * Dataflow equations for SSA form:
 *   - GEN[n] = { v | v is an SSA value used by n }
 *   - KILL[n] = { n } if n produces a non-void SSA value, ∅ otherwise
 *   - IN[n]  = (OUT[n] - KILL[n]) ∪ GEN[n]
 *   - OUT[n] = ⋃ IN[s] for all CFG successors s of n
 *
 * Note: This analysis computes liveness of SSA registers only. It does NOT
 * track memory location liveness. Memory liveness requires a separate analysis
 * operating on MemorySSA def-use chains or explicit memory location lattices.
 *
 * @param f The function to analyze
 * @return DataFlowResult containing live SSA value sets for each instruction
 */
std::unique_ptr<DataFlowResult>
runLiveVariablesAnalysis(llvm::Function *f,
                         const DebugConfig &DebugCfg = DebugConfig{});

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_ANALYSES_INTRA_LIVEVARIABLES_H_
