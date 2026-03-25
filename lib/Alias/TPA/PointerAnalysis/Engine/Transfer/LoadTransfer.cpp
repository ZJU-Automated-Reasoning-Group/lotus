// Implementation of Load transfer functions.
//
// Handles the evaluation of load instructions (p = *q).
// Reads from the memory Store to update the points-to set of the destination
// pointer 'p'.
//
// Logic:
// 1. Look up the points-to set of 'q' in the Environment.
// 2. For each memory object 'obj' that 'q' points to:
//    a. Look up the content of 'obj' in the Store.
// 3. Union all found contents to form the new points-to set for 'p'.
// 4. Update the Environment with this new set (Strong Update on Env for 'p').

#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"

namespace tpa {

// Helper to perform the load operation given a source pointer and store.
// Returns the accumulated points-to set read from memory.
PtsSet TransferFunction::loadFromPointer(const Pointer *ptr,
                                         const Store &store) {
  assert(ptr != nullptr);

  const auto *uObj = MemoryManager::getUniversalObject();
  auto const &srcSet = globalState.getEnv().lookup(ptr);

  // Bug fix: if the source pointer's points-to set is empty, the pointer is
  // not yet resolved (fixpoint iteration artifact). Return the empty set so
  // the caller can defer evaluation rather than conservatively returning
  // Universal, which would cause cascading imprecision.
  if (srcSet.empty())
    return PtsSet::getEmptySet();

  std::vector<PtsSet> srcSets;
  srcSets.reserve(srcSet.size());

  // Iterate over all possible memory locations we are loading from
  for (const auto *obj : srcSet) {
    // Loading through Universal means the result is also Universal.
    if (obj == uObj)
      return PtsSet::getSingletonSet(uObj);

    auto const &objSet = store.lookup(obj);
    if (!objSet.empty()) {
      srcSets.emplace_back(objSet);
      // Optimization: if any object contains Universal, the result is Universal
      if (objSet.has(uObj))
        return PtsSet::getSingletonSet(uObj);
    }
  }

  return PtsSet::mergeAll(srcSets);
}

// Visitor method for Load nodes.
void TransferFunction::evalLoadNode(const ProgramPoint &pp,
                                    EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &loadNode = static_cast<const LoadCFGNode &>(*pp.getCFGNode());

  auto &ptrManager = globalState.getPointerManager();
  const auto *srcPtr = ptrManager.getPointer(ctx, loadNode.getSrc());

  // If source pointer hasn't been seen yet, we can't load anything.
  if (srcPtr == nullptr)
    return;

  // assert(srcPtr != nullptr && "LoadNode is evaluated before its src operand
  // becomes available");

  // Create or get the pointer representation for the destination register
  const auto *dstPtr = ptrManager.getOrCreatePointer(ctx, loadNode.getDest());

  // Perform the load
  const auto resSet = loadFromPointer(srcPtr, *localState);

  // Update the Environment for the destination pointer.
  // Since 'dstPtr' is an SSA value (register), we can always do a Strong Update
  // because it has a single definition.
  auto envChanged = globalState.getEnv().strongUpdate(dstPtr, resSet);

  // If the environment changed, we need to propagate to top-level users
  if (envChanged)
    addTopLevelSuccessors(pp, evalResult);

  // Always propagate the store to memory-level successors
  addMemLevelSuccessors(pp, *localState, evalResult);
}

} // namespace tpa
