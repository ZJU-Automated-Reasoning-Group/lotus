// Implementation of SemiSparseProgram.
//
// SemiSparseProgram acts as the top-level container for the program being
// analyzed. It bridges the LLVM Module with the analysis-specific
// representations (CFGs, TypeMaps).
//
// Key Responsibilities:
// 1. Maintain the mapping between LLVM Functions and TPA CFGs.
// 2. Identify the entry point of the program (main).
// 3. Track address-taken functions (needed for indirect call resolution).
// 4. Hold the global TypeMap.

#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"

#include <llvm/IR/Module.h>

using namespace llvm;

namespace tpa {

// Constructor. Scans the module to populate the list of address-taken
// functions. These functions are potential targets for indirect calls where the
// target is unknown (Universal).
SemiSparseProgram::SemiSparseProgram(const llvm::Module &m) : module(m) {
  for (auto const &f : module) {
    if (f.hasAddressTaken())
      addrTakenFuncList.push_back(&f);
  }
}

// Lazy initialization of CFGs.
CFG &SemiSparseProgram::getOrCreateCFGForFunction(const llvm::Function &f) {
  auto itr = cfgMap.find(&f);
  if (itr == cfgMap.end())
    itr = cfgMap.emplace(&f, f).first;
  return itr->second;
}

// Const version of getOrCreate (mutable internal via const_cast).
CFG &SemiSparseProgram::getOrCreateCFGForFunction(
    const llvm::Function &f) const {
  return const_cast<SemiSparseProgram *>(this)->getOrCreateCFGForFunction(f);
}

const CFG *SemiSparseProgram::getCFGForFunction(const llvm::Function &f) const {
  auto itr = cfgMap.find(&f);
  if (itr == cfgMap.end())
    return nullptr;
  else
    return &itr->second;
}

// Determines the main entry point for the analysis.
const CFG *SemiSparseProgram::getEntryCFG() const {
  // 1. Try to find the standard "main" function.
  if (auto *mainFunc = module.getFunction("main"))
    return &getOrCreateCFGForFunction(*mainFunc);

  // 2. Fallback for libraries or partial modules:
  //    Pick the first non-declaration, non-intrinsic function.
  //    This is a heuristic to allow analysis to proceed even without a main.
  for (auto const &f : module) {
    if (f.isDeclaration())
      continue;
    if (f.isIntrinsic())
      continue;
    return &getOrCreateCFGForFunction(f);
  }

  // No suitable entry found.
  return nullptr;
}

} // namespace tpa
