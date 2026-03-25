/**
 * @file DeleteCalls.h
 * @brief Pass for deleting specific function calls
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_DELETE_CALLS_H
#define LOTUS_VERIFICATION_TRANSFORM_DELETE_CALLS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class DeleteCallsPass
 * @brief Deletes direct calls to specified functions
 *
 * This pass:
 * - Removes calls to functions specified via command-line option
 * - Replaces return values with UndefValue
 * - Useful for removing unwanted function calls during verification
 */
class DeleteCallsPass : public llvm::FunctionPass {
public:
  static char ID;

  DeleteCallsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_DELETE_CALLS_H
