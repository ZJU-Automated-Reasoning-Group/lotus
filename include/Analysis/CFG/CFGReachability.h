#ifndef ANALYSIS_CFG_CFGREACHABILITY_H
#define ANALYSIS_CFG_CFGREACHABILITY_H

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

// Do NOT use `using namespace llvm` in headers — use explicit llvm:: qualifiers
// throughout to avoid polluting includers' namespaces.

/// CFGReachability - Provides reachability analysis for basic blocks and
/// instructions within a function's control flow graph.
///
/// Uses a lazy, demand-driven backward BFS: for each destination block queried,
/// a backward BFS from that block marks all predecessor blocks that can reach
/// it. Results are cached in a per-destination bit-vector.
///
/// Validity: the object is tied to the function's block layout at construction
/// time. If blocks are added, removed, or the function is otherwise modified
/// after construction, the object must be discarded and rebuilt — calling
/// reachable() on a stale object is undefined behaviour. Use isValid() to
/// check whether a block still belongs to the analysed function.
///
/// Memory: O(N^2) in the number of basic blocks (one bit-vector row per
/// destination that is actually queried).
///
/// Thread safety: reachable() is safe to call concurrently from multiple
/// threads. Internal state is protected by a mutex.
class CFGReachability {
private:
  using ReachableVec = llvm::BitVector;

  /// The function this object was built for. Used by isValid().
  llvm::Function *AnalyzedFunction;

  /// One bit per block: has analyze() been run for this destination?
  ReachableVec AnalyzedVec;

  /// ReachableMatrix[dstID][srcID] == true  iff  src can reach dst.
  /// Stored as a vector of bit-vectors to avoid raw new[]/delete[].
  std::vector<ReachableVec> ReachableMatrix;

  /// ID mapping
  std::vector<llvm::BasicBlock *> ID2BB;
  std::map<llvm::BasicBlock *, unsigned> BB2ID;

  /// Protects AnalyzedVec and ReachableMatrix for concurrent reachable() calls.
  mutable std::mutex CacheMutex;

public:
  explicit CFGReachability(llvm::Function *F);

  // Non-copyable: the mutex member is not copyable, and copying a large
  // reachability cache is almost never intentional.
  CFGReachability(const CFGReachability &) = delete;
  CFGReachability &operator=(const CFGReachability &) = delete;

  // Not movable: std::mutex is not movable, so the implicitly-deleted move
  // constructor/assignment cannot be defaulted.  Explicitly delete them to
  // suppress the -Wdefaulted-function-deleted warning and make the intent
  // clear.
  CFGReachability(CFGReachability &&) = delete;
  CFGReachability &operator=(CFGReachability &&) = delete;

  ~CFGReachability() = default;

  /// Returns true if \p BB belongs to the function this object was built for.
  /// Use this to detect stale objects after IR modifications.
  bool isValid(llvm::BasicBlock *BB) const { return BB2ID.count(BB) != 0; }

  /// Returns true if there is a path from \p From to \p To in the CFG.
  /// Both blocks must belong to the function passed at construction;
  /// passing a block from a different (or modified) function is undefined
  /// behaviour — use isValid() to guard against this.
  bool reachable(llvm::BasicBlock *From, llvm::BasicBlock *To);

  /// Returns true if there is a path from instruction \p From to instruction
  /// \p To.
  ///
  /// Same-block case:
  ///   • If From comes before or at To in the block  → true (straight-line).
  ///   • If To comes before From in the block        → true iff the block can
  ///     reach itself (i.e., it is in a cycle / has a back-edge to itself).
  bool reachable(llvm::Instruction *From, llvm::Instruction *To);

private:
  /// Backward BFS from \p ToBB: marks every block that has a path to ToBB.
  /// Must be called with CacheMutex held.
  void analyze(llvm::BasicBlock *ToBB);
};

using CFGReachabilityRef = std::shared_ptr<CFGReachability>;

#endif // ANALYSIS_CFG_CFGREACHABILITY_H
