/**
 * @file GetTestTargets.h
 * @brief Pass for finding test targets for test generation
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_ANALYSIS_GET_TEST_TARGETS_H
#define LOTUS_VERIFICATION_ANALYSIS_GET_TEST_TARGETS_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace analysis {

/**
 * @class GetTestTargetsPass
 * @brief Finds exit blocks that can serve as test targets
 *
 * This pass traverses the CFG starting from main and identifies exit blocks
 * (blocks with no successors and no calls) that can be used as test targets.
 * It inserts calls to __SYMBIOTIC_test_target* functions at these locations.
 */
class GetTestTargetsPass : public llvm::ModulePass {
public:
  static char ID;

  GetTestTargetsPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace analysis
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_ANALYSIS_GET_TEST_TARGETS_H
