/*
 *
 * Author: rainoftime
 */
#include "Dataflow/NPA/Analyses/Intra/ReachableBlocks.h"

#include "Dataflow/NPA/LLVM/BitVectorProblem.h"

namespace npa {

class ReachableInfo : public BitVectorProblem {
public:
  // 1 bit: 0=Unreachable, 1=Reachable
  unsigned getBitWidth() const override { return 1; }

  // GEN: Basic Blocks always generate "reachability" for themselves if reached
  // Actually, standard formulation:
  // OUT[B] = IN[B]
  // IN[B] = U OUT[P]
  // Entry = 1
  //
  // In BitVectorSolver:
  // OUT[B] = GEN[B] U (IN[B] - KILL[B])
  // We want OUT[B] = IN[B], so GEN=0, KILL=0.
  llvm::APInt getGen(const llvm::BasicBlock *BB) const override {
    return llvm::APInt(1, 0);
  }

  llvm::APInt getKill(const llvm::BasicBlock *BB) const override {
    return llvm::APInt(1, 0);
  }

  bool isForward() const override { return true; }

  // Boundary (Entry) is Reachable (1)
  llvm::APInt getBoundaryVal() const override { return llvm::APInt(1, 1); }
};

std::set<const llvm::BasicBlock *>
ReachableBlocks::run(llvm::Function &F, SolverStrategy strategy,
                     LinearStrategy linearStrategy) {
  ReachableInfo info;
  auto result = BitVectorSolver::run(F, info, strategy, linearStrategy);

  std::set<const llvm::BasicBlock *> reachable;
  for (auto &entry : result.OUT) {
    // If bit 0 is set, block is reachable
    if (entry.second[0]) {
      reachable.insert(entry.first);
    }
  }
  return reachable;
}

} // namespace npa
