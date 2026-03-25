/**
 * @file CheckModule.h
 * @brief Analysis pass for checking module features
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_ANALYSIS_CHECK_MODULE_H
#define LOTUS_VERIFICATION_ANALYSIS_CHECK_MODULE_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace analysis {

/**
 * @class CheckModulePass
 * @brief Checks whether the module contains specific features
 *
 * This analysis pass checks for:
 * - Calls to specific functions (via -detect-calls option)
 * - Calls via function pointers
 */
class CheckModulePass : public llvm::ModulePass {
public:
  static char ID;

  CheckModulePass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }
};

} // namespace analysis
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_ANALYSIS_CHECK_MODULE_H
