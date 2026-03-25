// Implementation of the PrecisionLossTracker.
//
// This component identifies the sources of precision loss in the pointer
// analysis. It traces back from a given set of "interesting" pointers (e.g.,
// query results) to find where the points-to sets became imprecise (too large
// or containing Universal).
//
// Algorithm:
// 1. Start with a set of target pointers/program points.
// 2. Perform a backward traversal of the value dependence graph (using
// ValueDependenceTracker).
// 3. At merge points (e.g., PHI nodes, function returns), compare the precision
// of
//    incoming values vs. the result.
// 4. If a merge causes significant precision loss (e.g., merging precise set
// with Universal),
//    flag the source of the imprecise value.

#include "Alias/TPA/PointerAnalysis/Precision/PrecisionLossTracker.h"

#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/WorkList.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/MemoryManager.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/Pointer.h"
#include "Alias/TPA/PointerAnalysis/MemoryModel/PointerManager.h"
#include "Alias/TPA/PointerAnalysis/Precision/TrackerGlobalState.h"
#include "Alias/TPA/PointerAnalysis/Precision/ValueDependenceTracker.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFGNode.h"
#include "Alias/TPA/PointerAnalysis/Program/SemiSparseProgram.h"

#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>

using namespace context;
using namespace llvm;

namespace tpa {

// Helper to resolve the parent function of a Value.
static const Function *getFunction(const Value *val) {
  if (const auto *arg = dyn_cast<Argument>(val))
    return arg->getParent();
  else if (const auto *inst = dyn_cast<Instruction>(val))
    return inst->getParent()->getParent();
  else
    return nullptr;
}

// Converts abstract Pointers to concrete ProgramPoints (CFG nodes).
PrecisionLossTracker::ProgramPointList
PrecisionLossTracker::getProgramPointsFromPointers(const PointerList &ptrs) {
  ProgramPointList list;
  list.reserve(ptrs.size());

  for (const auto *ptr : ptrs) {
    const auto *value = ptr->getValue();

    if (const auto *func = getFunction(value)) {
      const auto *cfg =
          globalState.getSemiSparseProgram().getCFGForFunction(*func);
      assert(cfg != nullptr);

      const auto *node = cfg->getCFGNodeForValue(value);
      assert(node != nullptr);
      list.push_back(ProgramPoint(ptr->getContext(), node));
    }
  }

  return list;
}

namespace {

// Internal worker class for the backward tracking traversal.
class ImprecisionTracker {
private:
  TrackerGlobalState &globalState;

  PtsSet getPtsSet(const Context *, const Value *);
  bool morePrecise(const PtsSet &, const PtsSet &);

  void checkCalleeDependencies(const ProgramPoint &, ProgramPointSet &);
  void checkCallerDependencies(const ProgramPoint &, ProgramPointSet &);

public:
  ImprecisionTracker(TrackerGlobalState &s) : globalState(s) {}

  void runOnWorkList(BackwardWorkList &workList);
};

PtsSet ImprecisionTracker::getPtsSet(const Context *ctx, const Value *val) {
  const auto *ptr = globalState.getPointerManager().getPointer(ctx, val);
  assert(ptr != nullptr);
  return globalState.getEnv().lookup(ptr);
}

// Main backward traversal loop.
void ImprecisionTracker::runOnWorkList(BackwardWorkList &workList) {
  while (!workList.empty()) {
    const auto pp = workList.dequeue();
    // Avoid cycles
    if (!globalState.insertVisitedLocation(pp))
      continue;

    // Find where the value at 'pp' comes from (backward dependency)
    auto deps = ValueDependenceTracker(globalState.getCallGraph(),
                                       globalState.getSemiSparseProgram())
                    .getValueDependencies(pp);

    const auto *node = pp.getCFGNode();
    // Special handling for inter-procedural boundaries
    if (node->isCallNode())
      checkCalleeDependencies(pp, deps);
    else if (node->isEntryNode())
      checkCallerDependencies(pp, deps);

    // Continue tracking backwards
    for (auto const &succ : deps)
      workList.enqueue(succ);
  }
}

// Heuristic to compare precision of two points-to sets.
// Returns true if lhs is "significantly" more precise than rhs.
// Criteria:
// 1. If rhs has Universal (unknown), and lhs doesn't, lhs is more precise.
// 2. Otherwise, smaller set size is considered more precise.
bool ImprecisionTracker::morePrecise(const PtsSet &lhs, const PtsSet &rhs) {
  const auto *uObj = MemoryManager::getUniversalObject();
  if (rhs.has(uObj))
    return !lhs.has(uObj);

  return lhs.size() < rhs.size();
}

// Checks dependencies at a function call (tracking back from return value to
// callee return). If a specific callee returns a much more precise set than
// what is observed at the call site (which is the union of all callees), then
// other callees must be polluting the result.
void ImprecisionTracker::checkCalleeDependencies(const ProgramPoint &pp,
                                                 ProgramPointSet &deps) {
  assert(pp.getCFGNode()->isCallNode());
  const auto *callNode = static_cast<const CallCFGNode *>(pp.getCFGNode());
  const auto *dstVal = callNode->getDest();
  if (dstVal == nullptr)
    return;

  // The points-to set observed at the call site (merged result)
  const auto dstSet = getPtsSet(pp.getContext(), dstVal);
  assert(!dstSet.empty());

  ProgramPointSet newSet;
  bool needPrecision = false;

  // Check each potential callee's return value
  for (auto const &retPoint : deps) {
    assert(retPoint.getCFGNode()->isReturnNode());
    const auto *retNode =
        static_cast<const ReturnCFGNode *>(retPoint.getCFGNode());
    const auto *retVal = retNode->getReturnValue();
    assert(retVal != nullptr);

    const auto retSet = getPtsSet(retPoint.getContext(), retVal);
    assert(!retSet.empty());

    // If a callee returns a set that is significantly more precise than the
    // merged result, then the merge operation at this call site is a source of
    // precision loss.
    if (morePrecise(retSet, dstSet))
      needPrecision = true;
    else
      newSet.insert(retPoint);
  }

  // If we detected precision loss here, mark this call site as a culprit.
  if (needPrecision) {
    globalState.addImprecisionSource(pp);
    // Focus tracking on the imprecise paths? Or precise ones?
    // Logic here seems to swap deps to newSet (the ones that are NOT more
    // precise, i.e., the polluters?) This implies we want to track down where
    // the *bad* values came from.
    deps.swap(newSet);
  }
}

// Checks dependencies at a function entry (tracking back from parameter to
// caller arguments). If individual callers pass more precise argument sets than
// the merged parameter set (which is the union of all caller arguments), then
// merging causes precision loss.
void ImprecisionTracker::checkCallerDependencies(const ProgramPoint &pp,
                                                 ProgramPointSet &deps) {
  assert(pp.getCFGNode()->isEntryNode());
  const auto *entryNode = static_cast<const EntryCFGNode *>(pp.getCFGNode());
  const auto &func = entryNode->getFunction();
  const auto *funcCtx = pp.getContext();

  // Check each pointer-typed parameter of the function
  unsigned paramIdx = 0;
  for (const auto &arg : func.args()) {
    if (!arg.getType()->isPointerTy())
      continue;

    // Get the merged parameter points-to set (union of all caller arguments)
    const auto *paramPtr =
        globalState.getPointerManager().getPointer(funcCtx, &arg);
    if (paramPtr == nullptr)
      continue;

    const auto paramSet = getPtsSet(funcCtx, &arg);
    if (paramSet.empty())
      continue;

    ProgramPointSet newSet;
    bool needPrecision = false;

    // Check each caller's argument precision
    for (auto const &callerPP : deps) {
      assert(callerPP.getCFGNode()->isCallNode());
      const auto *callNode =
          static_cast<const CallCFGNode *>(callerPP.getCFGNode());

      // Find the corresponding argument at this call site
      // We need to match parameter index to argument index (skipping
      // non-pointer args)
      unsigned argIdx = 0;
      unsigned pointerArgIdx = 0;
      for (auto argItr = callNode->begin(); argItr != callNode->end();
           ++argItr, ++argIdx) {
        const auto *argVal = *argItr;
        if (!argVal->getType()->isPointerTy())
          continue;

        if (pointerArgIdx == paramIdx) {
          // This is the argument corresponding to the parameter we're checking
          const auto *argPtr = globalState.getPointerManager().getPointer(
              callerPP.getContext(), argVal);
          if (argPtr != nullptr) {
            const auto argSet = getPtsSet(callerPP.getContext(), argVal);
            if (!argSet.empty()) {
              // If this caller's argument is more precise than the merged
              // parameter, then merging causes precision loss
              if (morePrecise(argSet, paramSet))
                needPrecision = true;
              else
                newSet.insert(callerPP);
            }
          }
          break;
        }
        ++pointerArgIdx;
      }
    }

    // If we detected precision loss for this parameter, mark the entry point as
    // imprecision source
    if (needPrecision) {
      globalState.addImprecisionSource(pp);
      // Focus tracking on the imprecise callers (the ones that are NOT more
      // precise)
      deps.swap(newSet);
      // Only check the first parameter that shows precision loss to avoid
      // redundant checks
      break;
    }

    ++paramIdx;
  }
}

} // namespace

// Entry point for the tracker.
ProgramPointSet
PrecisionLossTracker::trackImprecision(const PointerList &ptrs) {
  ProgramPointSet ppSet;

  const auto ppList = getProgramPointsFromPointers(ptrs);
  auto workList = BackwardWorkList();
  for (auto const &pp : ppList)
    workList.enqueue(pp);

  TrackerGlobalState trackerState(
      globalState.getPointerManager(), globalState.getMemoryManager(),
      globalState.getSemiSparseProgram(), globalState.getEnv(),
      globalState.getCallGraph(), globalState.getExternalPointerTable(), ppSet);

  ImprecisionTracker(trackerState).runOnWorkList(workList);

  return ppSet;
}

} // namespace tpa
