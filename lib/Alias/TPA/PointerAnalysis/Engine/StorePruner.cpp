// Implementation of the StorePruner.
//
// Store pruning is an optimization technique used at function calls.
// Instead of passing the entire memory store to the callee, we only pass the
// portion of the store that is "reachable" from the function arguments and
// global variables.
//
// This reduces the size of the store propagated through the analysis, improving
// performance and potentially precision (by hiding irrelevant parts of the
// heap/stack).
//
// Algorithm:
// 1. Root Identification: Collect all memory objects pointed to by arguments
// and all global objects.
// 2. Reachability Analysis: Traverse the memory graph (Store) starting from
// roots to find all reachable objects.
// 3. Filtering: Create a new Store containing only the reachable objects.

#include "Alias/TPA/PointerAnalysis/Engine/StorePruner.h"

#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryObject.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFGNode.h"
#include "Alias/TPA/Util/IO/PointerAnalysis/Printer.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

// Heuristic to determine if an object is "globally accessible" by default.
// Stack and Heap objects are generally only accessible if a pointer to them is
// passed explicitly. Global objects (and functions) are always accessible.
static bool isAccessible(const MemoryObject *obj) {
  return !(obj->isStackObject() || obj->isHeapObject());
}

// Computes the initial set of objects that are immediately reachable at the
// call site. Roots include:
// 1. Objects pointed to by the function arguments.
// 2. Global objects (universally reachable).
StorePruner::ObjectSet StorePruner::getRootSet(const Store &store,
                                               const ProgramPoint &pp) {
  ObjectSet ret;

  const auto *ctx = pp.getContext();
  auto const &callNode = static_cast<const CallCFGNode &>(*pp.getCFGNode());

  // 1. Add objects reachable via arguments
  for (const auto *argVal : callNode) {
    const auto *argPtr = ptrManager.getPointer(ctx, argVal);
    // argPtr may be nullptr for values not tracked by the pointer manager
    // (e.g., constants, undef values, or global values not yet registered).
    // In such cases, we skip them as they don't contribute to the root set.
    if (argPtr == nullptr)
      continue;

    auto argSet = env.lookup(argPtr);
    ret.insert(argSet.begin(), argSet.end());
  }

  // 2. Add implicitly accessible objects (Globals) found in the current store
  for (auto const &mapping : store) {
    if (isAccessible(mapping.first))
      ret.insert(mapping.first);
  }

  return ret;
}

// Performs a BFS/DFS to find the transitive closure of reachable objects in the
// store.
void StorePruner::findAllReachableObjects(const Store &store,
                                          ObjectSet &reachableSet) {
  // Worklist initialized with the roots
  std::vector<const MemoryObject *> currWorkList(reachableSet.begin(),
                                                 reachableSet.end());
  std::vector<const MemoryObject *> nextWorkList;
  nextWorkList.reserve(reachableSet.size());
  ObjectSet exploredSet;

  while (!currWorkList.empty()) {
    for (const auto *obj : currWorkList) {
      // Avoid cycles and redundant processing
      if (!exploredSet.insert(obj).second)
        continue;

      // Add to reachable set (redundant if coming from roots, but needed for
      // transitive)
      reachableSet.insert(obj);

      // 1. Trace structural reachability:
      //    Objects that are physically part of 'obj' (e.g., fields of a struct)
      // Bug fix: the original code used 'break' when an already-explored object
      // was encountered, which incorrectly stopped processing the remaining
      // fields of the struct. For example, if field[1] was already explored but
      // field[2] was not, field[2] would be silently skipped, causing the
      // pruned store to miss reachable objects and producing unsound results.
      // Fix: use 'continue' to skip already-explored objects but keep
      // processing.
      auto offsetObjs = memManager.getReachablePointerObjects(obj, false);
      for (const auto *oObj : offsetObjs)
        if (!exploredSet.count(oObj))
          nextWorkList.push_back(oObj);
      // else: already explored, skip but continue to next field

      // 2. Trace pointer reachability:
      //    Objects pointed to by pointers stored inside 'obj'
      auto pSet = store.lookup(obj);
      for (const auto *pObj : pSet)
        if (!exploredSet.count(pObj))
          nextWorkList.push_back(pObj);
    }

    currWorkList.clear();
    currWorkList.swap(nextWorkList);
  }
}

// Constructs a new Store containing ONLY the reachable objects.
// Unreachable objects are pruned (not included in the result).
Store StorePruner::filterStore(const Store &store,
                               const ObjectSet &reachableSet) {
  Store ret;
  for (auto const &mapping : store) {
    if (reachableSet.count(mapping.first))
      ret.strongUpdate(mapping.first, mapping.second);
    // else: skip unreachable objects (pruning)
  }
  return ret;
}

// Main entry point for store pruning.
Store StorePruner::pruneStore(const Store &store, const ProgramPoint &pp) {
  assert(pp.getCFGNode()->isCallNode() &&
         "Pruning can only happen on call node!");

  auto reachableSet = getRootSet(store, pp);
  findAllReachableObjects(store, reachableSet);
  return filterStore(store, reachableSet);
}

} // namespace tpa