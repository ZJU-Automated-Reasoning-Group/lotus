/**
 * @file FlattenLoops.h
 * @brief Pass for flattening nested loops into non-nested loops
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_FLATTEN_LOOPS_H
#define LOTUS_VERIFICATION_TRANSFORM_FLATTEN_LOOPS_H

#include "llvm/Analysis/LoopPass.h"

namespace llvm {
class Loop;
class LPPassManager;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class FlattenLoopsPass
 * @brief Flattens nested loops into non-nested loops using flags
 *
 * This pass transforms nested loops into a single loop with a flag variable
 * that controls which loop body to execute. Useful for simplifying control flow
 * for analysis tools.
 */
class FlattenLoopsPass : public llvm::LoopPass {
public:
  static char ID;

  FlattenLoopsPass() : LoopPass(ID) {}

  bool runOnLoop(llvm::Loop *L, llvm::LPPassManager &LPM) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<llvm::LoopInfoWrapperPass>();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_FLATTEN_LOOPS_H
