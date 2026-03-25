#ifndef DEADLOCK_CHECKER_H
#define DEADLOCK_CHECKER_H

#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"
#include "Analysis/Concurrency/MHP/HappensBeforeAnalysis.h"
#include "Analysis/Concurrency/MHP/IMHPAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"

#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace concurrency {

/**
 * @brief Specialized checker for deadlock detection (Goblint-style)
 *
 * Detects potential deadlocks by:
 * 1. Building a lock-order graph (edge L' -> L when L is acquired while holding
 * L')
 * 2. Finding cycles in the graph via DFS
 * 3. Reporting a deadlock only when all acquire events in the cycle may happen
 *    in parallel (MHP), so the cycle can actually occur at runtime.
 */
class DeadlockChecker {
public:
  explicit DeadlockChecker(llvm::Module &module,
                           mhp::LockSetAnalysis *locksetAnalysis,
                           mhp::IMHPAnalysis *mhpAnalysis,
                           lotus::HappensBeforeAnalysis *hbAnalysis,
                           ThreadAPI *threadAPI);

  /**
   * @brief Check for deadlocks in the module
   * @return Vector of deadlock reports
   */
  std::vector<ConcurrencyBugReport> checkDeadlocks();

private:
  // Analysis components
  llvm::Module &m_module;
  mhp::LockSetAnalysis *m_locksetAnalysis;
  mhp::IMHPAnalysis *m_mhpAnalysis;
  lotus::HappensBeforeAnalysis *m_hbAnalysis;
  ThreadAPI *m_threadAPI;

  // Goblint-style: lock-order graph with events (from_lock -> [(to_lock,
  // acquire_inst)])
  using LockOrderEdge = std::pair<mhp::LockID, const llvm::Instruction *>;
  using LockOrderGraph =
      std::unordered_map<mhp::LockID, std::vector<LockOrderEdge>>;

  void buildLockOrderGraph(LockOrderGraph &graph) const;
  bool cycleCanHappenInParallel(
      const std::vector<const llvm::Instruction *> &acquireInsts) const;
  std::vector<std::vector<std::pair<mhp::LockID, const llvm::Instruction *>>>
  findLockOrderCycles(const LockOrderGraph &graph) const;

  bool isLockOperation(const llvm::Instruction *inst) const;
  mhp::LockID getLockID(const llvm::Instruction *inst) const;
  std::string getLockDescription(mhp::LockID lock) const;
  const llvm::Instruction *
  findMatchingUnlock(const llvm::Instruction *lockInst) const;
  std::vector<ConcurrencyBugReport> detectLostWakeups() const;
  std::vector<ConcurrencyBugReport> detectBarrierDivergence() const;
  bool isSameValue(const llvm::Value *lhs, const llvm::Value *rhs) const;
  std::string describeValue(const llvm::Value *value) const;
};

} // namespace concurrency

#endif // DEADLOCK_CHECKER_H
