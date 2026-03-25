// Implementation of Call transfer functions.
//
// This file handles the evaluation of function calls, which is the most complex
// part of the pointer analysis. It handles:
// 1. Call Graph Construction: Dynamic discovery of callee functions (for
// indirect calls).
// 2. Argument Passing: Mapping actual arguments (caller) to formal parameters
// (callee).
// 3. Return Value Passing: Mapping return values (callee) to call sites
// (caller).
// 4. Context Sensitivity: Creating new contexts for callees (via
// KLimitContext).
// 5. External Calls: Handling calls to external/library functions using
// annotations.

#include "Alias/TPA/Context/ContextPolicy.h"
#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/StorePruner.h"
#include "Alias/TPA/PointerAnalysis/Engine/TransferFunction.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"
#include "Alias/TPA/Util/IO/PointerAnalysis/Printer.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

// Helper to count the number of pointer-typed arguments in a function.
// Used to verify signature matching for indirect calls.
static inline size_t countPointerArguments(const llvm::Function *f) {
  size_t ret = 0;
  for (auto &arg : f->args()) {
    if (arg.getType()->isPointerTy())
      ++ret;
  }
  return ret;
};

// Resolves potential target functions from a points-to set of a function
// pointer. Handles the case where the function pointer points to the "Universal
// Object" by conservatively matching signatures of all address-taken functions.
std::vector<const llvm::Function *>
TransferFunction::findFunctionInPtsSet(PtsSet pSet,
                                       const CallCFGNode &callNode) {
  auto callees = std::vector<const llvm::Function *>();

  // Two cases here
  if (pSet.has(MemoryManager::getUniversalObject())) {
    // If funSet contains unknown location, then we can't really derive callees
    // based on the points-to set. Instead, guess callees based on the number of
    // arguments (type-based matching approximation).
    auto defaultTargets = globalState.getSemiSparseProgram().addr_taken_funcs();
    std::copy_if(defaultTargets.begin(), defaultTargets.end(),
                 std::back_inserter(callees), [&callNode](const Function *f) {
                   bool isArgMatch =
                       f->isVarArg() ||
                       countPointerArguments(f) == callNode.getNumArgument();
                   bool isRetMatch = (f->getReturnType()->isPointerTy()) !=
                                     (callNode.getDest() == nullptr);
                   return isArgMatch && isRetMatch;
                 });
  } else {
    // Precise resolution: the object in the points-to set IS the function.
    for (const auto *obj : pSet) {
      if (obj->isFunctionObject())
        callees.emplace_back(obj->getAllocSite().getFunction());
    }
  }

  return callees;
}

// Top-level resolver for call targets.
// Fetches the points-to set of the called value and delegates to
// findFunctionInPtsSet.
std::vector<const llvm::Function *>
TransferFunction::resolveCallTarget(const context::Context *ctx,
                                    const CallCFGNode &callNode) {
  auto callees = std::vector<const llvm::Function *>();

  const auto *funPtr = globalState.getPointerManager().getPointer(
      ctx, callNode.getFunctionPointer());
  if (funPtr != nullptr) {
    auto const &funSet = globalState.getEnv().lookup(funPtr);
    if (!funSet.empty())
      callees = findFunctionInPtsSet(funSet, callNode);
  }

  return callees;
}

// Collects the points-to sets of all actual arguments at the call site.
// Returns a vector of PtsSet, one per pointer parameter. If an argument's
// points-to set is not yet available (empty), we use the empty set as a
// placeholder rather than aborting early. This prevents premature fixpoints
// where a callee is never analyzed because one argument is not yet resolved.
std::vector<PtsSet>
TransferFunction::collectArgumentPtsSets(const context::Context *ctx,
                                         const CallCFGNode &callNode,
                                         size_t numParams) {
  std::vector<PtsSet> result;
  result.reserve(numParams);

  auto &ptrManager = globalState.getPointerManager();
  auto &env = globalState.getEnv();
  auto argItr = callNode.begin();
  const auto argEnd = callNode.end();
  for (auto i = 0u; i < numParams; ++i) {
    if (argItr == argEnd)
      break;
    const auto *argPtr = ptrManager.getPointer(ctx, *argItr);
    if (argPtr == nullptr) {
      // Argument not yet registered in the pointer manager; use empty set as
      // placeholder so we still attempt to analyze the callee.
      result.emplace_back(PtsSet::getEmptySet());
      ++argItr;
      continue;
    }

    auto const &pSet = env.lookup(argPtr);
    // Bug fix: previously an empty points-to set caused early termination of
    // the loop, which made evalCallArguments return (false, false) and skip
    // the entire call. This could cause premature fixpoints if the argument's
    // set is computed later. Now we include the empty set as a placeholder;
    // updateParameterPtsSets will simply perform a no-op weak update for it,
    // and the call will be re-evaluated when the argument's set changes.
    result.emplace_back(pSet);
    ++argItr;
  }

  return result;
}

// Updates the formal parameters of the callee in the new context.
// Performs a "weak update" to merge points-to sets from different call sites.
// Returns true if the environment changed.
bool TransferFunction::updateParameterPtsSets(
    const FunctionContext &fc, const std::vector<PtsSet> &argSets) {
  auto changed = false;

  auto &ptrManager = globalState.getPointerManager();
  auto &env = globalState.getEnv();
  const auto *newCtx = fc.getContext();
  const auto *paramItr = fc.getFunction()->arg_begin();
  for (auto const &pSet : argSets) {
    assert(paramItr != fc.getFunction()->arg_end());
    while (!paramItr->getType()->isPointerTy()) {
      ++paramItr;
      assert(paramItr != fc.getFunction()->arg_end());
    }
    const Argument *paramVal = paramItr;
    ++paramItr;

    const auto *paramPtr = ptrManager.getOrCreatePointer(newCtx, paramVal);
    changed |= env.weakUpdate(paramPtr, pSet);
  }

  return changed;
}

// Evaluates argument passing.
// Returns {isValid, envChanged}.
std::pair<bool, bool>
TransferFunction::evalCallArguments(const context::Context *ctx,
                                    const CallCFGNode &callNode,
                                    const FunctionContext &fc) {
  const auto numParams = countPointerArguments(fc.getFunction());
  if (callNode.getNumArgument() < numParams)
    return std::make_pair(false, false);
  const auto argSets = collectArgumentPtsSets(ctx, callNode, numParams);
  if (argSets.size() < numParams)
    return std::make_pair(false, false);

  auto envChanged = updateParameterPtsSets(fc, argSets);
  return std::make_pair(true, envChanged);
}

// Handling for internal (defined in the module) function calls.
// 1. Evaluates argument passing.
// 2. Prunes the store (optimizes by removing irrelevant objects for the
// callee).
// 3. Propagates execution to the callee's Entry node.
// 4. Also propagates to the "next" instruction in the caller (to handle return
// flow
//    or parallel execution assumption).
void TransferFunction::evalInternalCall(const context::Context *ctx,
                                        const CallCFGNode &callNode,
                                        const FunctionContext &fc,
                                        EvalResult &evalResult,
                                        bool callGraphUpdated) {
  const auto *tgtCFG =
      globalState.getSemiSparseProgram().getCFGForFunction(*fc.getFunction());
  assert(tgtCFG != nullptr);
  const auto *tgtEntryNode = tgtCFG->getEntryNode();

  bool isValid, envChanged;
  std::tie(isValid, envChanged) = evalCallArguments(ctx, callNode, fc);
  if (!isValid)
    return;

  // If the environment changed (new args) or it's a new edge, we must
  // re-evaluate the callee's entry point.
  if (envChanged || callGraphUpdated) {
    evalResult.addTopLevelProgramPoint(
        ProgramPoint(fc.getContext(), tgtEntryNode));
  }

  // Pass the store to the callee.
  // We use StorePruner to reduce the size of the passed store,
  // keeping only objects reachable from the arguments and globals.
  auto prunedStore =
      StorePruner(globalState.getEnv(), globalState.getPointerManager(),
                  globalState.getMemoryManager())
          .pruneStore(*localState, ProgramPoint(ctx, &callNode));
  auto &newStore = evalResult.getNewStore(std::move(prunedStore));
  evalResult.addMemLevelProgramPoint(
      ProgramPoint(fc.getContext(), tgtEntryNode), newStore);

  // Force enqueuing the direct successors of the call in the caller.
  // This ensures that even if the callee doesn't return immediately,
  // the caller continues analysis (approximation).
  if (!tgtCFG->doesNotReturn())
    addMemLevelSuccessors(ProgramPoint(ctx, &callNode), *localState,
                          evalResult);
}

// Main visitor method for Call nodes.
void TransferFunction::evalCallNode(const ProgramPoint &pp,
                                    EvalResult &evalResult) {
  const auto *ctx = pp.getContext();
  auto const &callNode = static_cast<const CallCFGNode &>(*pp.getCFGNode());

  const auto callees = resolveCallTarget(ctx, callNode);
  if (callees.empty()) {
    // Fix #9: if the callee set is empty the function pointer's points-to set
    // has not been resolved yet (it will be populated later in the fixpoint
    // iteration). Call nodes are mem-level nodes and are only re-enqueued when
    // the store changes. But the function pointer's points-to set lives in the
    // Env (top-level), so a store-only re-enqueue may never happen.
    //
    // To avoid a premature fixpoint we need two things:
    // 1. Propagate the current store to mem-level successors so that when the
    //    function pointer is resolved (triggering a top-level re-evaluation of
    //    this node via the def-use chain), the store state at the successors is
    //    already up-to-date.
    // 2. Also enqueue the top-level successors (def-use users of the call's
    //    return value). Without this, if the call has a pointer-typed return
    //    value, its users will never be evaluated because they are only
    //    triggered by top-level changes, and no top-level change is recorded
    //    here (the return value's points-to set is empty/unknown).
    addMemLevelSuccessors(pp, *localState, evalResult);
    addTopLevelSuccessors(pp, evalResult);
    return;
  }

  for (const auto *f : callees) {
    // Update call graph first.
    // Create context for callee (policy: k-CFA everywhere or selective 0-CFA
    // for direct calls, k-CFA for indirect).
    const auto *callsite = callNode.getCallSite();
    const auto *newCtx = context::pushContextForCall(ctx, callsite);
    auto callTgt = FunctionContext(newCtx, f);
    bool callGraphUpdated = globalState.getCallGraph().insertEdge(
        ProgramPoint(ctx, &callNode), callTgt);

    // Check whether f is an external library call or internal function.
    if (f->isDeclaration())
      evalExternalCall(ctx, callNode, callTgt, evalResult);
    else
      evalInternalCall(ctx, callNode, callTgt, evalResult, callGraphUpdated);
  }
}

} // namespace tpa
