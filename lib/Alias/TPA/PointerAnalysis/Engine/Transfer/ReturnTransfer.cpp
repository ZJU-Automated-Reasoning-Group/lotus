#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

std::pair<bool, bool>
TransferFunction::evalReturnValue(const context::Context *ctx,
                                  const ReturnCFGNode &retNode,
                                  const ProgramPoint &retSite) {
  assert(retSite.getCFGNode()->isCallNode());
  auto const &callNode =
      static_cast<const CallCFGNode &>(*retSite.getCFGNode());

  const auto *retVal = retNode.getReturnValue();
  if (retVal == nullptr) {
    // Void function: no pointer value is returned. Do NOT write NullObject into
    // the call-site destination — a void return carries no pointer information.
    return std::make_pair(true, false);
  }

  const auto *dstVal = callNode.getDest();
  if (dstVal == nullptr)
    // Returned a value, but not used by the caller
    return std::make_pair(true, false);

  auto &ptrManager = globalState.getPointerManager();
  const auto *retPtr = ptrManager.getPointer(ctx, retVal);
  if (retPtr == nullptr)
    // Fix #3: Return value pointer not yet registered. Return (true, false)
    // instead of (false, false) so that evalReturn still propagates the store
    // to mem-level successors of the call site. Returning (false, false) caused
    // evalReturn to bail out entirely, dropping the store propagation and
    // potentially causing a premature fixpoint when the return value's
    // points-to set is computed later in the iteration.
    return std::make_pair(true, false);

  auto &env = globalState.getEnv();
  auto resSet = env.lookup(retPtr);
  if (resSet.empty())
    // Fix #3: Same reasoning — empty set means not yet resolved, not an error.
    // Propagate the store but report no env change.
    return std::make_pair(true, false);

  const auto *dstPtr =
      ptrManager.getOrCreatePointer(retSite.getContext(), dstVal);
  return std::make_pair(true, env.weakUpdate(dstPtr, resSet));
}

void TransferFunction::evalReturn(const context::Context *ctx,
                                  const ReturnCFGNode &retNode,
                                  const ProgramPoint &retSite,
                                  EvalResult &evalResult) {
  bool valid, envChanged;
  std::tie(valid, envChanged) = evalReturnValue(ctx, retNode, retSite);

  // Fix #3: valid is now always true (see evalReturnValue above), so this
  // guard is kept only for future-proofing. The key change is that we always
  // reach addMemLevelSuccessors, ensuring the store is propagated to the
  // call-site successors even when the return value is not yet resolved.
  if (!valid)
    return;
  if (envChanged)
    addTopLevelSuccessors(retSite, evalResult);
  addMemLevelSuccessors(retSite, *localState, evalResult);
}

void TransferFunction::evalReturnNode(const ProgramPoint &pp,
                                      EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &retNode = static_cast<const ReturnCFGNode &>(*pp.getCFGNode());

  // Bug fix: previously this hardcoded the string "main" to detect the program
  // entry point. This breaks for libraries, embedded programs, or any program
  // whose entry function is not named "main" (e.g., WinMain, _start, or a
  // user-specified entry). Instead, compare against the actual entry CFG
  // obtained from SemiSparseProgram, which already handles the fallback logic.
  const auto *entryCFG = globalState.getSemiSparseProgram().getEntryCFG();
  if (entryCFG != nullptr &&
      &retNode.getFunction() == &entryCFG->getFunction()) {
    // Return from the program entry point. Nothing to propagate.
    return;
  }

  // Merge back pruned mappings in store
  // auto prunedStore =
  // globalState.getStorePruner().lookupPrunedStore(FunctionContext(ctx,
  // &retNode.getFunction())); if (prunedStore != nullptr)
  //	evalResult.getStore().mergeWith(*prunedStore);

  for (auto retSite : globalState.getCallGraph().getCallers(
           FunctionContext(ctx, &retNode.getFunction())))
    evalReturn(ctx, retNode, retSite, evalResult);
}

} // namespace tpa
