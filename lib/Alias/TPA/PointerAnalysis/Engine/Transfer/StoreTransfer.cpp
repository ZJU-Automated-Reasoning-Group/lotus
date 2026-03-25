// Implementation of Store transfer functions.
//
// Handles the evaluation of store instructions (*p = q).
// Updates the memory Store based on the points-to sets of the pointers
// involved.
//
// Key Concepts:
// - Strong Update: Completely overwrites the points-to set of a memory object.
//   Possible only when the pointer points to a SINGLE, PRECISE memory object.
// - Weak Update: Adds new relations to the existing points-to set (union).
//   Used when the pointer may point to multiple objects or summary objects.

#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

namespace tpa {

// Performs a strong update on the store.
// Overwrites the content of 'obj' with 'pSet'.
void TransferFunction::strongUpdateStore(const MemoryObject *obj, PtsSet pSet,
                                         Store &store) {
  if (!obj->isSpecialObject())
    store.strongUpdate(obj, pSet);
  // TODO: in the else branch, report NULL-pointer dereference to the user
}

// Performs a weak update on the store.
// Merges 'srcSet' into the existing content of all objects in 'dstSet'.
void TransferFunction::weakUpdateStore(PtsSet dstSet, PtsSet srcSet,
                                       Store &store) {
  for (const auto *updateObj : dstSet) {
    if (!updateObj->isSpecialObject())
      store.weakUpdate(updateObj, srcSet);
  }
}

// Core logic for evaluating a store operation *dst = src.
void TransferFunction::evalStore(const Pointer *dst, const Pointer *src,
                                 const ProgramPoint &pp,
                                 EvalResult &evalResult) {
  auto &env = globalState.getEnv();

  // Look up what 'src' points to (the value being stored)
  auto srcSet = env.lookup(src);
  if (srcSet.empty())
    return;

  // Look up what 'dst' points to (the location being written to)
  auto dstSet = env.lookup(dst);
  if (dstSet.empty())
    return;

  // Create a new Store for the output of this node
  auto &store = evalResult.getNewStore(*localState);

  const auto *dstObj = *dstSet.begin();
  // Check conditions for Strong Update:
  // 1. Singleton set: We know exactly which object is being written.
  // 2. Not a summary object: The object represents a single concrete memory
  // location
  //    (not an array summary or heap abstraction that represents multiple
  //    locs).
  if (dstSet.size() == 1 && !dstObj->isSummaryObject())
    strongUpdateStore(dstObj, srcSet, store);
  else
    weakUpdateStore(dstSet, srcSet, store);

  // Propagate the updated store to successors
  addMemLevelSuccessors(pp, store, evalResult);
}

// Visitor method for Store nodes.
void TransferFunction::evalStoreNode(const ProgramPoint &pp,
                                     EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &storeNode = static_cast<const StoreCFGNode &>(*pp.getCFGNode());

  auto &ptrManager = globalState.getPointerManager();
  const auto *srcPtr = ptrManager.getPointer(ctx, storeNode.getSrc());
  const auto *dstPtr = ptrManager.getPointer(ctx, storeNode.getDest());

  if (srcPtr == nullptr || dstPtr == nullptr)
    return;

  evalStore(dstPtr, srcPtr, pp, evalResult);
}

} // namespace tpa
