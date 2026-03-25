/**
 * @file ClassifyInstructions.h
 * @brief Analysis pass for classifying instruction types
 * @author Migrated from Symbiotic
 */

#ifndef LOTUS_VERIFICATION_ANALYSIS_CLASSIFY_INSTRUCTIONS_H
#define LOTUS_VERIFICATION_ANALYSIS_CLASSIFY_INSTRUCTIONS_H

#include "llvm/Pass.h"

namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace lotus {
namespace verification {
namespace analysis {

/**
 * @class ClassifyInstructionsPass
 * @brief Classifies and reports instruction types in functions
 *
 * This analysis pass classifies instructions and prints statistics about:
 * - Stack arrays and variable-length arrays
 * - Memory allocation calls (malloc, calloc, realloc)
 * - Bit-wise and bit-shift operations
 */
class ClassifyInstructionsPass : public llvm::FunctionPass {
public:
  static char ID;

  ClassifyInstructionsPass() : FunctionPass(ID) {}

  bool runOnFunction(llvm::Function &F) override;
  bool doFinalization(llvm::Module &M) override;

  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }
};

} // namespace analysis
} // namespace verification
} // namespace lotus

#endif // LOTUS_VERIFICATION_ANALYSIS_CLASSIFY_INSTRUCTIONS_H
