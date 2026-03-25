//===-- Verification/Sifa/RegionMemory.h ----------------------------------===//
//
// Region-based memory model for Sifa value domains (IKOS/CLAM style).
// Reuses pointer/alias analyses from lib/Alias to resolve pointers to
// abstract regions (allocas, globals). Used by IntervalDomain and other
// value domains for sound Load/Store transfer when alias analysis is provided.
//
// Comparison: lib/Verification/clam uses HeapAbstraction (SeaDsa) which maps
// (function, pointer) -> one Region with RegionId, RegionInfo (type, bitwidth,
// is_sequence, is_heap), and optional singleton. Sifa uses a fixed region set
// (allocas + globals) and resolves pointer -> set of regions via AA, then
// joins over that set. See lib/Verification/Sifa/README.md for a short table.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_REGIONMEMORY_H
#define LOTUS_VERIFICATION_SIFA_REGIONMEMORY_H

#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Value.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

#include <vector>

namespace lotus {
namespace sifa {

/// Set of abstract regions: allocas in the function + globals in the module.
/// Region id = const Value* (AllocaInst or GlobalVariable).
inline std::vector<const llvm::Value *>
getRegionsForFunction(const llvm::Function &F) {
  std::vector<const llvm::Value *> regions;
  for (auto &I : F.getEntryBlock())
    if (llvm::isa<llvm::AllocaInst>(&I))
      regions.push_back(&I);
  const llvm::Module *M = F.getParent();
  if (M)
    for (const auto &G : M->globals())
      if (!G.isDeclaration())
        regions.push_back(&G);
  return regions;
}

/// Resolve pointer \p ptr to the set of regions it may point into.
/// Uses getPointsToSet when the AA backend supports it (e.g. SparrowAA);
/// otherwise falls back to mayAlias over \p regions. \p regions should be
/// from getRegionsForFunction(F) for the function containing the use of ptr.
inline void resolvePointerToRegions(lotus::AliasAnalysisWrapper *AA,
                                    const llvm::Value *ptr,
                                    llvm::ArrayRef<const llvm::Value *> regions,
                                    std::vector<const llvm::Value *> &out) {
  out.clear();
  if (!AA || !ptr || !ptr->getType()->isPointerTy())
    return;
  std::vector<const llvm::Value *> ptsSet;
  if (AA->getPointsToSet(ptr, ptsSet)) {
    for (const llvm::Value *v : ptsSet) {
      if (llvm::isa<llvm::AllocaInst>(v) || llvm::isa<llvm::GlobalVariable>(v))
        out.push_back(v);
    }
    if (!out.empty())
      return;
  }
  for (const llvm::Value *r : regions)
    if (AA->mayAlias(ptr, r))
      out.push_back(r);
}

} // namespace sifa
} // namespace lotus

#endif
