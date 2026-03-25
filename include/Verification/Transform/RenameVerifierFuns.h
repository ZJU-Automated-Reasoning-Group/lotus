/**
 * @file RenameVerifierFuns.h
 * @brief Pass for renaming verifier function calls with named versions
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_RENAME_VERIFIER_FUNS_H
#define LOTUS_VERIFICATION_TRANSFORM_RENAME_VERIFIER_FUNS_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class RenameVerifierFunsPass
 * @brief Renames __VERIFIER_nondet_* calls with named versions
 *
 * This pass replaces calls to __VERIFIER_nondet_* functions with calls to
 * _named versions that include variable name information from source code.
 * Requires source file access via command-line option.
 */
class RenameVerifierFunsPass : public llvm::ModulePass {
public:
  static char ID;

  RenameVerifierFunsPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_RENAME_VERIFIER_FUNS_H
