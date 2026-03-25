/**
 * @file CountInstr.h
 * @brief Analysis pass for counting module statistics
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_ANALYSIS_COUNT_INSTR_H
#define LOTUS_VERIFICATION_ANALYSIS_COUNT_INSTR_H

#include "llvm/Pass.h"

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace analysis {

/**
 * @class CountInstrPass
 * @brief Counts and prints statistics about the module
 *
 * This analysis pass counts and reports:
 * - Number of global variables
 * - Number of functions
 * - Number of basic blocks
 * - Number of instructions
 */
class CountInstrPass : public llvm::ModulePass {
public:
  static char ID;

  CountInstrPass() : ModulePass(ID) {}

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }
};

} // namespace analysis
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_ANALYSIS_COUNT_INSTR_H
