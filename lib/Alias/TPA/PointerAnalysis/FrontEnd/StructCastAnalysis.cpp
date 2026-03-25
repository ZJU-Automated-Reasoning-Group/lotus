// Implementation of StructCastAnalysis.
//
// This analysis identifies "compatible" struct types by analyzing BitCast
// instructions. It constructs a map of (SrcType -> DstType) where a cast
// occurs.
//
// Purpose:
// To handle C-style polymorphism and type punning. If a pointer to StructA is
// cast to a pointer to StructB, the pointer analysis needs to merge the layouts
// or account for the aliasing between fields of StructA and StructB.
//
// Algorithm:
// 1. Collect all BitCast instructions in the module.
// 2. Build a graph of type casts.
// 3. Compute the Transitive Closure of the graph (if A->B and B->C, then A->C).
// 4. Extract only the Struct types involved.

#include "Alias/TPA/PointerAnalysis/FrontEnd/Type/StructCastAnalysis.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

using namespace llvm;

namespace tpa {

namespace {

class CastMapBuilder {
private:
  const Module &module;
  CastMap &structCastMap;

  void collectCast(const Value &, CastMap &);
  CastMap collectAllCasts();
  void computeTransitiveClosure(CastMap &);
  void extractStructs(CastMap &);

public:
  CastMapBuilder(const Module &m, CastMap &c) : module(m), structCastMap(c) {}

  void buildCastMap();
};

void CastMapBuilder::collectCast(const Value &value, CastMap &castMap) {
  if (const auto *bc = dyn_cast<BitCastOperator>(&value)) {
    auto *srcType = bc->getSrcTy();
    auto *dstType = bc->getDestTy();

    // Only care about pointer casts
    if (!srcType->isPointerTy() || !dstType->isPointerTy())
      return;

    // Filter out irrelevant casts (e.g. to/from void* in memcpy, though usually
    // we want those?) This specific check skips casts used *only* by
    // MemIntrinsics (memcpy/memset).
    if (bc->hasOneUse()) {
      const auto *user = *bc->user_begin();
      if (isa<MemIntrinsic>(user))
        return;
    }

    castMap.insert(srcType, dstType);
  }
}

// Scans globals and functions for bitcasts.
CastMap CastMapBuilder::collectAllCasts() {
  CastMap castMap;

  for (auto const &global : module.globals()) {
    if (global.hasInitializer())
      collectCast(*global.getInitializer(), castMap);
  }

  for (auto const &f : module)
    for (auto const &bb : f)
      for (auto const &inst : bb)
        collectCast(inst, castMap);

  return castMap;
}

// Computes transitive closure: if T1 casts to T2, and T2 casts to T3, then T1
// casts to T3.
//
// Bug fix: the previous implementation used a naive fixed-point loop that
// iterated over all mappings on every pass, giving O(n³) complexity in the
// number of types. For large programs with many struct types and casts (e.g.,
// the Linux kernel), this was extremely slow.
//
// Replacement: a worklist-based BFS that processes each newly-added edge
// exactly once. For each source type S, we maintain a worklist of types
// whose outgoing edges have not yet been propagated into S's reachable set.
// Each edge (S -> T) is processed at most once per source, giving O(n²) in
// the worst case (bounded by the number of distinct cast pairs).
void CastMapBuilder::computeTransitiveClosure(CastMap &castMap) {
  // For each source type, run a BFS over the cast graph to find all
  // transitively reachable types.
  for (auto &mapping : castMap) {
    auto &reachable = mapping.second;
    // Worklist: types whose successors we still need to explore.
    std::vector<Type *> worklist(reachable.begin(), reachable.end());

    while (!worklist.empty()) {
      Type *cur = worklist.back();
      worklist.pop_back();

      // Skip self-loops.
      if (cur == mapping.first)
        continue;

      auto itr = castMap.find(cur);
      if (itr == castMap.end())
        continue;

      for (auto *next : itr->second) {
        if (next == mapping.first)
          continue;
        // Only add to the worklist if this is a genuinely new reachable type.
        if (reachable.insert(next).second)
          worklist.push_back(next);
      }
    }
  }
}

// Filters the cast map to only include struct types.
void CastMapBuilder::extractStructs(CastMap &castMap) {
  for (auto const &mapping : castMap) {
    auto *lhs = mapping.first->getNonOpaquePointerElementType();
    if (!lhs->isStructTy())
      continue;

    auto &rhsSet = structCastMap.getOrCreateRHS(lhs);
    for (auto *dstType : mapping.second) {
      auto *rhs = dstType->getNonOpaquePointerElementType();
      if (!rhs->isStructTy())
        continue;
      rhsSet.insert(rhs);
    }
  }
}

void CastMapBuilder::buildCastMap() {
  auto allCastMap = collectAllCasts();
  computeTransitiveClosure(allCastMap);
  extractStructs(allCastMap);
}

} // namespace

CastMap StructCastAnalysis::runOnModule(const Module &module) {
  CastMap structCastMap;

  CastMapBuilder(module, structCastMap).buildCastMap();

  return structCastMap;
}

} // namespace tpa