// NameBlock pass assigns names to unnamed basic blocks for debugging purposes.
// This helps with debugging and analysis by ensuring all blocks have readable
// names.

#include "Transform/NameBlock.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "name-block"

// New Pass Manager entry point. Assigns names to all unnamed basic blocks.
PreservedAnalyses NameBlockPass::run(Module &M, ModuleAnalysisManager &) {
  for (auto &F : M) {
    unsigned BI = 0;
    for (auto &B : F) {
      if (!B.hasName())
        B.setName("B" + std::to_string(++BI));
    }
  }
  // Naming blocks doesn't affect analyses
  return PreservedAnalyses::all();
}