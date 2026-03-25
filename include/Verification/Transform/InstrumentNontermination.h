/**
 * @file InstrumentNontermination.h
 * @brief Pass for instrumenting loops with non-termination checks
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_TRANSFORM_INSTRUMENT_NONTERMINATION_H
#define LOTUS_VERIFICATION_TRANSFORM_INSTRUMENT_NONTERMINATION_H

#include "llvm/Analysis/LoopPass.h"

namespace llvm {
class Loop;
class LPPassManager;
} // namespace llvm

namespace lotus {
namespace verification {
namespace transform {

/**
 * @class InstrumentNonterminationPass
 * @brief Instruments loops with checks for state space cycles (non-termination)
 *
 * This pass instruments loops to detect cycles in the state space by comparing
 * variable values at loop entry and after each iteration. If all variables
 * remain unchanged, it indicates a potential infinite loop.
 */
class InstrumentNonterminationPass : public llvm::LoopPass {
public:
  static char ID;

  InstrumentNonterminationPass() : LoopPass(ID) {}

  bool runOnLoop(llvm::Loop *L, llvm::LPPassManager &LPM) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.addRequired<llvm::LoopInfoWrapperPass>();
  }
};

} // namespace transform
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_TRANSFORM_INSTRUMENT_NONTERMINATION_H
