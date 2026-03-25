/**
 * @file RemoveErrorCalls.h
 * @brief Pass for replacing error calls with assumptions/exits
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REMOVE_ERROR_CALLS_H
#define LOTUS_VERIFICATION_TRANSFORM_REMOVE_ERROR_CALLS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class RemoveErrorCallsPass
 * @brief Replaces __VERIFIER_error() and __assert_fail() calls with assumptions
 *
 * This pass replaces error calls with __VERIFIER_assume(0) or
 * __VERIFIER_exit(0), which silently abort the path without checking for leaks.
 * Useful for certain verification scenarios where error paths should be
 * eliminated.
 */
class RemoveErrorCallsPass : public llvm::FunctionPass {
public:
  static char ID;

  RemoveErrorCallsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REMOVE_ERROR_CALLS_H
