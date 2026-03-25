/**
 * @file PrepareOverflows.h
 * @brief Pass for instrumenting signed arithmetic with explicit overflow checks
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_PREPARE_OVERFLOWS_H
#define LOTUS_VERIFICATION_TRANSFORM_PREPARE_OVERFLOWS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class PrepareOverflowsPass
 * @brief Instruments signed arithmetic operations with explicit overflow checks
 *
 * This pass finds signed integer arithmetic operations (add, sub, mul) and
 * replaces them with overflow-checking intrinsics. If overflow is detected,
 * __VERIFIER_error() is called. This is useful for overflow property checking.
 */
class PrepareOverflowsPass : public llvm::FunctionPass {
public:
  static char ID;

  PrepareOverflowsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_PREPARE_OVERFLOWS_H
