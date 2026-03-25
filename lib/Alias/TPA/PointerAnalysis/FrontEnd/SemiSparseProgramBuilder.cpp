// Implementation of SemiSparseProgramBuilder.
//
// Driver for the frontend pipeline.
//
// Steps:
// 1. Type Analysis: Collects and analyzes all types in the module.
// 2. CFG Construction: Iterates over all functions to build their CFGs.
// 3. Result: Produces a `SemiSparseProgram` containing the IR ready for the
// Engine.

#include "Alias/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/CFGBuilder.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/TypeAnalysis.h"
#include "Alias/TPA/Util/Log.h"

#include <llvm/IR/Module.h>

using namespace llvm;

namespace tpa {

void SemiSparseProgramBuilder::buildCFGForFunction(SemiSparseProgram &ssProg,
                                                   const Function &f,
                                                   const TypeMap &typeMap) {
  auto &cfg = ssProg.getOrCreateCFGForFunction(f);
  CFGBuilder(cfg, typeMap).buildCFG(f);
}

SemiSparseProgram SemiSparseProgramBuilder::runOnModule(const Module &module) {
  SemiSparseProgram ssProg(module);

  // Process types
  LOG_INFO("Running type analysis on module...");
  auto typeMap = TypeAnalysis().runOnModule(module);
  unsigned typeCount = 0;
  for (auto it = typeMap.begin(); it != typeMap.end(); ++it) {
    typeCount++;
  }
  LOG_INFO("Type analysis completed: {} types in map", typeCount);

  // Translate functions to CFG
  unsigned numFunctions = 0;
  for (auto const &f : module) {
    if (f.isDeclaration())
      continue;
    numFunctions++;
  }
  LOG_INFO("Building CFGs for {} functions...", numFunctions);

  unsigned cfgCount = 0;
  for (auto const &f : module) {
    if (f.isDeclaration())
      continue;

    buildCFGForFunction(ssProg, f, typeMap);
    cfgCount++;
    if (cfgCount % 100 == 0) {
      LOG_INFO("  Built {} CFGs...", cfgCount);
    }
  }
  LOG_INFO("CFG construction completed: {} CFGs built", cfgCount);

  ssProg.setTypeMap(std::move(typeMap));
  return ssProg;
}

} // namespace tpa