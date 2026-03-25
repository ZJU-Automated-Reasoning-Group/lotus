// Implementation of the SemiSparsePropagator.
//
// The propagator is responsible for driving the worklist-based analysis.
// It takes evaluation results from transfer functions and propagates them
// to successor nodes in the semi-sparse CFG.
//
// Key Responsibilities:
// 1. Manage the worklist of program points to be visited.
// 2. Update the memoization table (Memo) with new analysis states.
// 3. Enqueue successors only when the analysis state changes (monotonicity).
// 4. Distinguish between "TopLevel" (pointer variables) and "MemLevel" (memory
// store) updates.

#include "Alias/TPA/PointerAnalysis/Engine/SemiSparsePropagator.h"

#include "Alias/TPA/PointerAnalysis/Engine/EvalResult.h"
#include "Alias/TPA/PointerAnalysis/Engine/GlobalState.h"
#include "Alias/TPA/PointerAnalysis/Engine/WorkList.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/TPA/PointerAnalysis/Support/Memo.h"
#include "Alias/TPA/Util/IO/PointerAnalysis/Printer.h"

#include <llvm/Support/raw_ostream.h>
using namespace llvm;

namespace tpa {

namespace {

// Checks if a CFG node is a "top-level" node.
// Top-level nodes (Alloc, Copy, Offset) only affect top-level pointer variables
// (Env). They do not read or write to the memory Store.
bool isTopLevelNode(const CFGNode *node) {
  return node->isAllocNode() || node->isCopyNode() || node->isOffsetNode();
}

} // namespace

// Updates the memoization table and returns true if the state changed.
// If the state for this program point has changed, the point is added to the
// worklist to propagate the new information to its successors.
bool SemiSparsePropagator::enqueueIfMemoChange(const ProgramPoint &pp,
                                               const Store &store) {
  if (memo.update(pp, store)) {
    workList.enqueue(pp);
    return true;
  } else
    return false;
}

// Propagates flow for top-level nodes (Alloc, Copy, Offset).
// Since these nodes don't modify the Store, we just unconditionally enqueue
// the successor program point. The Env updates happen in-place globally
// (managed by the TransferFunction/GlobalState).
void SemiSparsePropagator::propagateTopLevel(const EvalSuccessor &evalSucc) {
  // Top-level successors: no store merging, just enqueue
  workList.enqueue(evalSucc.getProgramPoint());
  // errs() << "\tENQ(T) " << evalSucc.getProgramPoint() << "\n";
}

// Propagates flow for memory-level nodes (Load, Store, Call, Ret).
// These nodes interact with the Store. We must check if the outgoing Store
// is different from what we've seen before at the successor point.
void SemiSparsePropagator::propagateMemLevel(const EvalSuccessor &evalSucc) {
  // Mem-level successors: store merging, enqueue if memo changed
  const auto *node = evalSucc.getProgramPoint().getCFGNode();
  assert(!isTopLevelNode(node));
  assert(evalSucc.getStore() != nullptr);

  // Check if the store state has changed for this program point.
  // If yes, update the memo table and add to worklist.
  bool enqueued =
      enqueueIfMemoChange(evalSucc.getProgramPoint(), *evalSucc.getStore());

  // if (enqueued)
  //	errs() << "\tENQ(M) " << evalSucc.getProgramPoint() << "\n";
}

// Main propagation entry point.
// Iterates over all successors produced by the transfer function evaluation
// and dispatches them to the appropriate propagation logic.
void SemiSparsePropagator::propagate(const EvalResult &evalResult) {
  for (auto const &evalSucc : evalResult) {
    if (evalSucc.isTopLevel())
      propagateTopLevel(evalSucc);
    else
      propagateMemLevel(evalSucc);
  }
}

} // namespace tpa
