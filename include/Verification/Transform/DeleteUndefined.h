/**
 * @file DeleteUndefined.h
 * @brief Pass for deleting calls to undefined functions and replacing with
 * nondet
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_DELETE_UNDEFINED_H
#define LOTUS_VERIFICATION_TRANSFORM_DELETE_UNDEFINED_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class DeleteUndefinedPass
 * @brief Deletes calls to undefined functions and replaces non-void undefined
 * functions with nondet
 *
 * This pass:
 * - Removes calls to undefined void functions
 * - Replaces undefined non-void functions with verifier.nondet.undef.* calls
 * - Preserves calls to verifier functions and standard library functions
 *
 * Useful for preparing programs with missing function definitions for
 * verification.
 */
class DeleteUndefinedPass : public llvm::ModulePass {
public:
  static char ID;

  DeleteUndefinedPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_DELETE_UNDEFINED_H
