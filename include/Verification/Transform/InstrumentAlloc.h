/**
 * @file InstrumentAlloc.h
 * @brief Pass for instrumenting malloc/calloc calls with verifier functions
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_INSTRUMENT_ALLOC_H
#define LOTUS_VERIFICATION_TRANSFORM_INSTRUMENT_ALLOC_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class InstrumentAllocPass
 * @brief Replaces malloc/calloc calls with __VERIFIER_malloc/__VERIFIER_calloc
 *
 * This pass instruments memory allocation calls to use verifier functions
 * that can track memory allocation for verification purposes.
 */
class InstrumentAllocPass : public llvm::FunctionPass {
public:
  static char ID;

  InstrumentAllocPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

/**
 * @class InstrumentAllocNeverFailsPass
 * @brief Same as InstrumentAllocPass but assumes allocations never fail
 *
 * Uses __VERIFIER_malloc0/__VERIFIER_calloc0 which assume allocations succeed.
 */
class InstrumentAllocNeverFailsPass : public llvm::FunctionPass {
public:
  static char ID;

  InstrumentAllocNeverFailsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_INSTRUMENT_ALLOC_H
