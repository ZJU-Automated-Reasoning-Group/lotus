/**
 * @file AndersenAA.cpp
 * @brief LLVM AliasAnalysis interface implementation for Andersen's pointer
 * analysis.
 *
 * This file implements the AliasAnalysis interface that LLVM uses to query
 * alias information. It wraps the underlying Andersen pointer analysis engine
 * and provides alias queries, points-to queries, and constant memory detection.
 *
 * @author rainoftime
 */
#include "Alias/SparrowAA/AndersenAA.h"

#include <vector>

#include <llvm/IR/Module.h>

using namespace llvm;

/**
 * @brief Check if a points-to set contains only a single specific node.
 *
 * @param set The points-to set to check
 * @param i The node index to check for
 * @return true if the set contains exactly one element and it equals i
 */
static inline bool isSetContainingOnly(const AndersPtsSet &set, NodeIndex i) {
  return (set.getSize() == 1) && (*set.begin() == i);
}

/**
 * @brief Determine the alias relationship between two LLVM values using
 * Andersen's analysis.
 *
 * First checks if the values are merged to the same node (MustAlias).
 * Then retrieves points-to sets and checks for null pointers (NoAlias).
 * Finally compares points-to sets for intersection
 * (MayAlias/MustAlias/NoAlias).
 *
 * @param v1 First value to check
 * @param v2 Second value to check
 * @return AliasResult indicating the alias relationship
 */
AliasResult AndersenAAResult::andersenAlias(const Value *v1, const Value *v2) {
  // NOTE: We intentionally do NOT return MustAlias based on merged node
  // representatives.  Two nodes are merged when they have the same points-to
  // set (pointer equivalence), not because they refer to the same memory
  // location.  Returning MustAlias for merged nodes is unsound and can cause
  // incorrect optimizations.

  AndersPtsSet s1, s2;
  if (!anders.getPointsToSet(v1, s1) || !anders.getPointsToSet(v2, s2))
    return AliasResult::MayAlias;

  NodeIndex nullObj = (anders.nodeFactory).getNullObjectNode();
  bool isNull1 = isSetContainingOnly(s1, nullObj);
  bool isNull2 = isSetContainingOnly(s2, nullObj);
  if (isNull1 || isNull2)
    return AliasResult::NoAlias;

  // MustAlias only when both sets are singletons pointing to the same object.
  if (s1.getSize() == 1 && s2.getSize() == 1 && *s1.begin() == *s2.begin())
    return AliasResult::MustAlias;

  if (s1.intersectWith(s2))
    return AliasResult::MayAlias;

  return AliasResult::NoAlias;
}

/**
 * @brief Determine the alias relationship between two memory locations.
 *
 * Implements the AliasAnalysis interface. Handles zero-sized locations,
 * strips pointer casts, and delegates to andersenAlias for the actual analysis.
 *
 * @param l1 First memory location
 * @param l2 Second memory location
 * @return AliasResult indicating the alias relationship
 */
AliasResult AndersenAAResult::alias(const MemoryLocation &l1,
                                    const MemoryLocation &l2) {
  if (l1.Size == 0 || l2.Size == 0)
    return AliasResult::NoAlias;

  const Value *v1 = (l1.Ptr)->stripPointerCasts();
  const Value *v2 = (l2.Ptr)->stripPointerCasts();

  if (!v1->getType()->isPointerTy() || !v2->getType()->isPointerTy())
    return AliasResult::NoAlias;

  if (v1 == v2)
    return AliasResult::MustAlias;

  return andersenAlias(v1, v2);
}

/**
 * @brief Check if a memory location points to constant memory.
 *
 * Retrieves the points-to set and verifies that all targets are constant
 * global variables or the null object. Used by LLVM optimizations that
 * can assume memory is immutable.
 *
 * @param loc The memory location to check
 * @param orLocal If true, also consider local constant memory (unused)
 * @return true if all points-to targets are constant, false otherwise
 */
bool AndersenAAResult::pointsToConstantMemory(const MemoryLocation &loc,
                                              bool orLocal) {
  AndersPtsSet ptsSet;
  if (!anders.getPointsToSet(loc.Ptr, ptsSet))
    return false;

  for (auto const &idx : ptsSet) {
    if (const Value *val = (anders.nodeFactory).getValueForNode(idx)) {
      if (!isa<GlobalValue>(val) || (isa<GlobalVariable>(val) &&
                                     !cast<GlobalVariable>(val)->isConstant()))
        return false;
    } else {
      if (idx != (anders.nodeFactory).getNullObjectNode())
        return false;
    }
  }

  return true;
}

/**
 * @brief Construct an AndersenAAResult with default context policy.
 *
 * @param m The module to analyze
 */
AndersenAAResult::AndersenAAResult(const Module &m)
    : anders(m, getSelectedAndersenContextPolicy()) {}

/**
 * @brief Construct an AndersenAAResult with a specific context policy.
 *
 * @param m The module to analyze
 * @param policy The context sensitivity policy to use
 */
AndersenAAResult::AndersenAAResult(const Module &m, ContextPolicy policy)
    : anders(m, policy) {}

/**
 * @brief Construct an AndersenAAResult with k-callsite context sensitivity.
 *
 * @param m The module to analyze
 * @param kCallSite The k value for k-callsite context sensitivity (0/1/2)
 */
AndersenAAResult::AndersenAAResult(const Module &m, unsigned kCallSite)
    : anders(m, makeContextPolicy(kCallSite)) {}

// New Pass Manager implementation
AnalysisKey AndersenAA::Key;

/**
 * @brief Run the AndersenAA analysis pass on a module.
 *
 * @param M The module to analyze
 * @param ModuleAnalysisManager The analysis manager (unused)
 * @return AndersenAAResult containing the analysis results
 */
AndersenAAResult AndersenAA::run(Module &M, ModuleAnalysisManager &) {
  return AndersenAAResult(M);
}