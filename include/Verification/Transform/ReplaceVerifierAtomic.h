/**
 * @file ReplaceVerifierAtomic.h
 * @brief Pass for replacing verifier atomic function names
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REPLACE_VERIFIER_ATOMIC_H
#define LOTUS_VERIFICATION_TRANSFORM_REPLACE_VERIFIER_ATOMIC_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class ReplaceVerifierAtomicPass
 * @brief Renames __VERIFIER_atomic_* functions to __symbiotic_atomic_*
 *
 * This pass:
 * - Renames __VERIFIER_atomic_begin to __symbiotic_atomic_begin
 * - Renames __VERIFIER_atomic_end to __symbiotic_atomic_end
 * - Workaround for Nidhugg backend compatibility
 */
class ReplaceVerifierAtomicPass : public llvm::ModulePass {
public:
  static char ID;

  ReplaceVerifierAtomicPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REPLACE_VERIFIER_ATOMIC_H
