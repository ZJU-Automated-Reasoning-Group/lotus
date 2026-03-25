/**
 * @file FindExits.h
 * @brief Pass for finding exit blocks and adding exit calls
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_FIND_EXITS_H
#define LOTUS_VERIFICATION_TRANSFORM_FIND_EXITS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class FindExitsPass
 * @brief Finds exit blocks and adds calls to exit functions
 *
 * This pass:
 * - Finds blocks with no successors (exit blocks)
 * - Inserts calls to __VERIFIER_silent_exit or __VERIFIER_exit
 * - Optionally replaces __VERIFIER_assume with __INSTR_check_assume
 */
class FindExitsPass : public llvm::FunctionPass {
public:
  static char ID;

  FindExitsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_FIND_EXITS_H
