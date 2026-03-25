/**
 * @file Unrolling.h
 * @brief Pass for unrolling loops with termination handling
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_UNROLLING_H
#define LOTUS_VERIFICATION_TRANSFORM_UNROLLING_H

#include "llvm/Analysis/LoopPass.h"

namespace llvm {
class Loop;
class LPPassManager;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class UnrollingPass
 * @brief Unrolls loops a specified number of times with optional termination
 *
 * This pass:
 * - Unrolls loops a configurable number of times
 * - Optionally adds termination paths with __VERIFIER_assume(0) for bounded
 * verification
 * - Useful for bounded model checking and verification scenarios
 */
class UnrollingPass : public llvm::LoopPass {
public:
  static char ID;

  UnrollingPass() : LoopPass(ID) {}

  bool runOnLoop(llvm::Loop *L, llvm::LPPassManager &LPM) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<llvm::LoopInfoWrapperPass>();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_UNROLLING_H
