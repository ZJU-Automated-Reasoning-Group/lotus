/**
 * @file DummyMarker.h
 * @brief Pass for marking allocations to prevent optimization
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_DUMMY_MARKER_H
#define LOTUS_VERIFICATION_TRANSFORM_DUMMY_MARKER_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class DummyMarkerPass
 * @brief Inserts dummy calls to prevent optimization removal
 *
 * This pass inserts calls to __symbiotic_keep_ptr after malloc/calloc calls
 * to prevent LLVM optimizations from removing these allocations.
 */
class DummyMarkerPass : public llvm::FunctionPass {
public:
  static char ID;

  DummyMarkerPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_DUMMY_MARKER_H
