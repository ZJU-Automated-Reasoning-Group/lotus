/**
 * @file BreakInfiniteLoops.h
 * @brief Pass for breaking infinite loops by adding exit edges
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_BREAK_INFINITE_LOOPS_H
#define LOTUS_VERIFICATION_TRANSFORM_BREAK_INFINITE_LOOPS_H

#include "llvm/Analysis/LoopPass.h"

namespace llvm {
class Loop;
class LPPassManager;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class BreakInfiniteLoopsPass
 * @brief Transforms loops with no exit to loops that have an exit edge
 *
 * This pass finds syntactically infinite loops (like while(1){}) and adds
 * a conditional exit edge that is never executed, making the loop analyzable
 * by tools that require exit edges.
 */
class BreakInfiniteLoopsPass : public llvm::LoopPass {
public:
  static char ID;

  BreakInfiniteLoopsPass() : LoopPass(ID) {}

  bool runOnLoop(llvm::Loop *L, llvm::LPPassManager &LPM) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<llvm::LoopInfoWrapperPass>();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_BREAK_INFINITE_LOOPS_H
