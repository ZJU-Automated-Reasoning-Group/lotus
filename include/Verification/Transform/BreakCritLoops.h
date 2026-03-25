/**
 * @file BreakCritLoops.h
 * @brief Pass for breaking critical loops for better slicing
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_BREAK_CRIT_LOOPS_H
#define LOTUS_VERIFICATION_TRANSFORM_BREAK_CRIT_LOOPS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class BreakCritLoopsPass
 * @brief Breaks critical loops for better slicing and control dependence
 * analysis
 *
 * This pass identifies basic blocks that jump back to themselves via critical
 * edges (from simplifycfg pass) and splits these blocks to improve control
 * dependence computation. This is useful before running PDG-based slicing.
 */
class BreakCritLoopsPass : public llvm::FunctionPass {
public:
  static char ID;

  BreakCritLoopsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_BREAK_CRIT_LOOPS_H
