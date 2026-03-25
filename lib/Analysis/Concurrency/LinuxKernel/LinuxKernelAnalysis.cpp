/**
 * @file LinuxKernelAnalysis.cpp
 * @brief Linux Kernel Analysis Coordinator Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/LinuxKernel/LinuxKernelAnalysis.h"

#include <unordered_map>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace kernel {

void LinuxKernelAnalysis::runAnalysis() {
  process_model_.analyzeModule();
  lock_analysis_.analyzeLocks();
  rcu_analysis_.analyzeRCU();
  wait_analysis_.analyzeWaits();

  results_.lock_deadlocks = lock_analysis_.findPotentialDeadlocks();
  results_.double_locks = lock_analysis_.findDoubleLocks();
  results_.unlock_without_lock = lock_analysis_.findUnlockWithoutLock();
  results_.lock_without_unlock = lock_analysis_.findLockWithoutUnlock();
  results_.lock_order_inversions = lock_analysis_.findLockOrderInversions();
  results_.mix_raw_and_cooked = lock_analysis_.findMixRawAndCookedLocks();
  results_.sleep_in_atomic = lock_analysis_.findSleepInSpinlock();
  results_.irq_save_restore_issues = lock_analysis_.findIrqSaveRestoreIssues();

  results_.rcu_without_grace_period =
      rcu_analysis_.findReadSideWithoutGracePeriod();
  results_.rcu_conflicts = rcu_analysis_.findRCUConflicts();
  results_.rcu_double_free = rcu_analysis_.findRCUDoubleFree();
  results_.deref_after_free = rcu_analysis_.findDerefAfterFree();

  results_.missing_wake_ups = wait_analysis_.findMissingWakeUps();
  results_.spurious_wake_ups = wait_analysis_.findSpuriousWakeUps();
  results_.missing_completion = wait_analysis_.findMissingCompletion();
  results_.double_completion = wait_analysis_.findDoubleCompletion();
  results_.timer_not_deleted = wait_analysis_.findTimerNotDeleted();
  results_.timer_use_after_delete = wait_analysis_.findTimerUseAfterDelete();
}

void LinuxKernelAnalysis::printResults(raw_ostream &OS) const {
  OS << "========================================\n";
  OS << "Linux Kernel Analysis Results\n";
  OS << "========================================\n\n";

  size_t total_ops = 0;
  const auto &counts = process_model_.getOperationKindCounts();
  for (const auto &pair : counts) {
    total_ops += pair.second;
  }

  OS << "Total kernel operations found: " << total_ops << "\n";
  OS << "Lock operations: " << getOperationCount(OperationKind::LOCK_ACQUIRE)
     << "/" << getOperationCount(OperationKind::LOCK_RELEASE) << "\n";
  OS << "RCU operations: " << getOperationCount(OperationKind::RCU_READ_LOCK)
     << "/" << getOperationCount(OperationKind::RCU_SYNC) << "\n";
  OS << "Wait operations: " << getOperationCount(OperationKind::WAIT_EVENT)
     << "/" << getOperationCount(OperationKind::WAKE_UP) << "\n\n";

  OS << "--- Lock Issues ---\n";
  OS << "Potential deadlocks: " << results_.lock_deadlocks.size() << "\n";
  OS << "Double locks: " << results_.double_locks.size() << "\n";
  OS << "Unlock without lock: " << results_.unlock_without_lock.size() << "\n";
  OS << "Lock without unlock: " << results_.lock_without_unlock.size() << "\n";
  OS << "Lock order inversions: " << results_.lock_order_inversions.size()
     << "\n\n";

  OS << "--- RCU Issues ---\n";
  OS << "Read without grace period: "
     << results_.rcu_without_grace_period.size() << "\n";
  OS << "RCU conflicts: " << results_.rcu_conflicts.size() << "\n";
  OS << "RCU double free: " << results_.rcu_double_free.size() << "\n\n";

  OS << "--- Wait/Completion Issues ---\n";
  OS << "Missing wake-ups: " << results_.missing_wake_ups.size() << "\n";
  OS << "Missing completions: " << results_.missing_completion.size() << "\n";
  OS << "Double completions: " << results_.double_completion.size() << "\n";
  OS << "Timers not deleted: " << results_.timer_not_deleted.size() << "\n\n";

  OS << "========================================\n";
}

size_t LinuxKernelAnalysis::getOperationCount(OperationKind kind) const {
  const auto &counts = process_model_.getOperationKindCounts();
  auto it = counts.find(kind);
  return it != counts.end() ? it->second : 0;
}

size_t LinuxKernelAnalysis::getLockCount() const {
  return process_model_.getLockInfoMap().size();
}

size_t LinuxKernelAnalysis::getRCUSectionCount() const {
  return rcu_analysis_.getReadSideSections().size();
}

size_t LinuxKernelAnalysis::getWaitQueueCount() const {
  return wait_analysis_.getWaitContexts().size();
}

} // namespace kernel
