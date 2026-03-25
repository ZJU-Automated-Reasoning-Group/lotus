/**
 * @file RemoveInfiniteLoops.h
 * @brief Pass for removing infinite loops (patterns like LABEL: goto LABEL)
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REMOVE_INFINITE_LOOPS_H
#define LOTUS_VERIFICATION_TRANSFORM_REMOVE_INFINITE_LOOPS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class RemoveInfiniteLoopsPass
 * @brief Removes infinite loops by replacing them with __VERIFIER_assume(0)
 *
 * This pass detects infinite loops (blocks that unconditionally jump to
 * themselves without side effects) and replaces them with __VERIFIER_assume(0)
 * followed by unreachable, effectively removing the loop.
 */
class RemoveInfiniteLoopsPass : public llvm::FunctionPass {
public:
  static char ID;

  RemoveInfiniteLoopsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REMOVE_INFINITE_LOOPS_H
