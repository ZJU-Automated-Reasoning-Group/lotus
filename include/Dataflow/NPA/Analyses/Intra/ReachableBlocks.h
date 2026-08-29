#ifndef NPA_REACHABLE_BLOCKS_H
#define NPA_REACHABLE_BLOCKS_H

#include "Dataflow/NPA/LLVM/BitVectorSolver.h"

#include <set>

#include <llvm/IR/Function.h>

namespace npa {

/**
 * @brief Simple Reachable Blocks analysis using NPA BitVector framework.
 */
class ReachableBlocks {
public:
  static std::set<const llvm::BasicBlock *>
  run(llvm::Function &F, SolverStrategy strategy = SolverStrategy::Newton,
      LinearStrategy linearStrategy = LinearStrategy::SCC);
};

} // namespace npa

#endif // NPA_REACHABLE_BLOCKS_H
