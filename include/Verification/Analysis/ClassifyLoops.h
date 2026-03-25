/**
 * @file ClassifyLoops.h
 * @brief Analysis pass for classifying loop types
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_ANALYSIS_CLASSIFY_LOOPS_H
#define LOTUS_VERIFICATION_ANALYSIS_CLASSIFY_LOOPS_H

#include "llvm/Analysis/LoopPass.h"

namespace llvm {
class Loop;
class LPPassManager;
} // namespace llvm

namespace lotus {
namespace verification {
namespace analysis {

/**
 * @class ClassifyLoopsPass
 * @brief Classifies and reports loop characteristics
 *
 * This analysis pass detects and reports:
 * - Presence of loops
 * - Nested loops
 * - Non-terminating loops (no exit blocks)
 * - Irreducible loops
 */
class ClassifyLoopsPass : public llvm::LoopPass {
public:
  static char ID;

  ClassifyLoopsPass() : LoopPass(ID) {}

  bool runOnLoop(llvm::Loop *L, llvm::LPPassManager &LPM) override;
  bool doFinalization() override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<llvm::LoopInfoWrapperPass>();
  }
};

} // namespace analysis
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_ANALYSIS_CLASSIFY_LOOPS_H
