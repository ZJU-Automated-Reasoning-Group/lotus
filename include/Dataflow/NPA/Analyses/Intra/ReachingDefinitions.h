#ifndef NPA_REACHING_DEFINITIONS_H
#define NPA_REACHING_DEFINITIONS_H

#include "Dataflow/NPA/LLVM/BitVectorSolver.h"

#include <llvm/IR/Function.h>

namespace npa {

/**
 * @brief Standard Reaching Definitions analysis using NPA BitVector framework.
 */
class ReachingDefinitions {
public:
  static BitVectorSolver::Result
  run(llvm::Function &F, SolverStrategy strategy = SolverStrategy::Newton,
      LinearStrategy linearStrategy = LinearStrategy::SCC);
};

} // namespace npa

#endif // NPA_REACHING_DEFINITIONS_H
