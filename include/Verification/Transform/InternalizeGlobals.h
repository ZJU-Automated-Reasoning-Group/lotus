/**
 * @file InternalizeGlobals.h
 * @brief Pass for internalizing external globals with nondeterministic
 * initialization
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_INTERNALIZE_GLOBALS_H
#define LOTUS_VERIFICATION_TRANSFORM_INTERNALIZE_GLOBALS_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class InternalizeGlobalsPass
 * @brief Internalizes external global variables and initializes them with
 * nondet values
 *
 * This pass finds external global variables (without initializers) and:
 * - Creates initializers for them
 * - Inserts calls to __VERIFIER_make_nondet at the beginning of main
 * - Handles pointer types by creating pointed-to objects
 * - Skips standard library globals (stdin, stderr, stdout, optind, optarg)
 */
class InternalizeGlobalsPass : public llvm::ModulePass {
public:
  static char ID;

  InternalizeGlobalsPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_INTERNALIZE_GLOBALS_H
