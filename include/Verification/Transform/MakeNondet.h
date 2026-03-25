/**
 * @file MakeNondet.h
 * @brief Pass for replacing input functions with nondeterministic values
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_MAKE_NONDET_H
#define LOTUS_VERIFICATION_TRANSFORM_MAKE_NONDET_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class MakeNondetPass
 * @brief Replaces selected input functions with nondeterministic value
 * generators
 *
 * This pass replaces calls to specified functions (e.g., rand(), getchar(),
 * scanf()) with calls to verifier.nondet.input.* functions. This is useful
 * for abstracting away I/O and random number generation in verification.
 */
class MakeNondetPass : public llvm::ModulePass {
public:
  static char ID;

  MakeNondetPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_MAKE_NONDET_H
