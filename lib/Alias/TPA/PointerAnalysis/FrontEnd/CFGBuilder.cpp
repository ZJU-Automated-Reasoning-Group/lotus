// Implementation of CFGBuilder.
//
// Driver class for the CFG construction process for a single function.
// Orchestrates the translation and simplification pipeline.
//
// Steps:
// 1. Translate Instructions: Use InstructionTranslator.
// 2. Build Basic Block CFG: Use FunctionTranslator.
// 3. Simplify CFG: Use CFGSimplifier.
// 4. Finalize: Build value mappings for lookups.

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/CFGBuilder.h"

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/CFGSimplifier.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/FunctionTranslator.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/InstructionTranslator.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"

#include <llvm/IR/DataLayout.h>

using namespace llvm;

namespace tpa {

CFGBuilder::CFGBuilder(CFG &c, const TypeMap &t) : cfg(c), typeMap(t) {}

void CFGBuilder::buildCFG(const Function &llvmFunc) {
  auto dataLayout = DataLayout(llvmFunc.getParent());

  // Phase 1: Translate LLVM instructions to TPA nodes
  auto instTranslator = InstructionTranslator(cfg, typeMap, dataLayout);

  // Phase 2: Connect nodes to form control flow graph and def-use chains
  FunctionTranslator(cfg, instTranslator).translateFunction(llvmFunc);

  // Phase 3: Simplify the graph (remove identity copies, etc.)
  CFGSimplifier().simplify(cfg);

  // Phase 4: Build helper maps for querying
  cfg.buildValueMap();
}

} // namespace tpa