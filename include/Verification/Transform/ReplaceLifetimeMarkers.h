/**
 * @file ReplaceLifetimeMarkers.h
 * @brief Pass for replacing lifetime markers with verifier scope calls
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_REPLACE_LIFETIME_MARKERS_H
#define LOTUS_VERIFICATION_TRANSFORM_REPLACE_LIFETIME_MARKERS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class ReplaceLifetimeMarkersPass
 * @brief Replaces lifetime.start and lifetime.end intrinsics with verifier
 * calls
 *
 * This pass replaces LLVM lifetime markers with calls to __VERIFIER_scope_enter
 * and __VERIFIER_scope_leave, making lifetime information visible to
 * verification tools.
 */
class ReplaceLifetimeMarkersPass : public llvm::FunctionPass {
public:
  static char ID;

  ReplaceLifetimeMarkersPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_REPLACE_LIFETIME_MARKERS_H
