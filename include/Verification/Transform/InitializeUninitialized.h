/**
 * @file InitializeUninitialized.h
 * @brief Pass for initializing stack allocas with nondeterministic values
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_INITIALIZE_UNINITIALIZED_H
#define LOTUS_VERIFICATION_TRANSFORM_INITIALIZE_UNINITIALIZED_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class InitializeUninitializedPass
 * @brief Initializes stack-allocated variables with nondeterministic values
 *
 * This pass finds all alloca instructions in function entry blocks and
 * initializes them with calls to verifier.nondet.init.* functions. This
 * is useful for verification where uninitialized variables should be
 * treated as nondeterministic.
 */
class InitializeUninitializedPass : public llvm::ModulePass {
public:
  static char ID;

  InitializeUninitializedPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_INITIALIZE_UNINITIALIZED_H
