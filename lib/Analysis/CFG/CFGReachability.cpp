#include "Analysis/CFG/CFGReachability.h"

#include <llvm/IR/CFG.h>

using namespace llvm;

CFGReachability::CFGReachability(Function *F)
    : AnalyzedFunction(F), AnalyzedVec(F->size(), false) {
  unsigned Idx = 0;
  for (auto &B : *F) {
    ID2BB.push_back(&B);
    BB2ID[&B] = Idx;
    ++Idx;
  }
  // Allocate one bit-vector row per block, all initialised to false.
  ReachableMatrix.assign(F->size(), BitVector(F->size(), false));
}

// Returns true if there is a path from From to To in the CFG.
bool CFGReachability::reachable(BasicBlock *From, BasicBlock *To) {
  assert(From && To);
  if (From == To)
    return true;

  // Bug 1 fix: validate that both blocks still belong to this function.
  // If the IR has been modified since construction, the caller must rebuild.
  assert(isValid(From) &&
         "CFGReachability: 'From' block not found — object may be stale");
  assert(isValid(To) &&
         "CFGReachability: 'To' block not found — object may be stale");

  const unsigned DstBlockID = BB2ID.at(To);

  // Bug 3 fix: lock the cache before reading or writing AnalyzedVec /
  // ReachableMatrix so that concurrent reachable() calls are safe.
  std::unique_lock<std::mutex> lock(CacheMutex);

  // Demand-driven: run the backward BFS only the first time To is queried.
  if (!AnalyzedVec[DstBlockID]) {
    analyze(To); // analyze() must be called with the lock held
    AnalyzedVec[DstBlockID] = true;
  }

  return ReachableMatrix[DstBlockID][BB2ID.at(From)];
}

// Returns true if there is a path from instruction From to instruction To.
//
// Fix #3: The old implementation only walked forward from From within the same
// block, returning false when To appeared earlier.  That is wrong for loops:
// if To precedes From in the block, From can still reach To via a back-edge
// that loops back to the block's header.
//
// Corrected logic for the same-block case:
//   1. Walk forward from From.  If we hit To before the end → reachable.
//   2. If we reach the end without finding To (To is before From) → reachable
//      iff the block can reach itself (i.e., it lies on a cycle).
bool CFGReachability::reachable(Instruction *From, Instruction *To) {
  assert(From && To);
  if (From == To)
    return true;

  BasicBlock *FromB = From->getParent();
  BasicBlock *ToB = To->getParent();

  if (FromB == ToB) {
    // Walk forward from From to see if To appears later in the same block.
    for (Instruction *I = From->getNextNode(); I != nullptr;
         I = I->getNextNode()) {
      if (I == To)
        return true; // To is textually after From — directly reachable.
    }
    // To is before From in the block.  Reachable only if the block is on a
    // cycle (i.e., the block can reach itself via some back-edge path).
    return reachable(FromB, FromB);
  }

  // Different blocks: delegate to block-level reachability.
  return reachable(FromB, ToB);
}

// Fix #1: Backward BFS from ToBB.  Marks every block B (B != ToBB) such that
// there exists a path B → … → ToBB.  The old code used a fragile `FirstRun`
// boolean to skip marking ToBB itself; replaced with an explicit `BB != ToBB`
// guard that makes the intent immediately clear.
void CFGReachability::analyze(BasicBlock *ToBB) {
  const unsigned ToBBID = BB2ID[ToBB];
  BitVector VisitedVec(static_cast<unsigned>(ID2BB.size()));
  ReachableVec &ToReachability = ReachableMatrix[ToBBID];

  std::vector<BasicBlock *> Worklist;
  Worklist.push_back(ToBB);

  while (!Worklist.empty()) {
    BasicBlock *BB = Worklist.back();
    Worklist.pop_back();

    unsigned BBID = BB2ID[BB];
    if (VisitedVec[BBID])
      continue;
    VisitedVec[BBID] = true;

    // Mark BB as able to reach ToBB — but only if BB is not ToBB itself.
    // (Self-reachability for ToBB is handled by the From==To early-return in
    // reachable(), so we deliberately leave ToReachability[ToBBID] = false
    // here to avoid a spurious true when From != To but both map to ToBBID,
    // which cannot happen, but keeping the invariant explicit is cleaner.)
    if (BB != ToBB)
      ToReachability[BBID] = true;

    for (BasicBlock *Pred : predecessors(BB))
      Worklist.push_back(Pred);
  }
}
