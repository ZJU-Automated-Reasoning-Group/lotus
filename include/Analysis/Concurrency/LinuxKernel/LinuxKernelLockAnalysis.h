/**
 * @file LinuxKernelLockAnalysis.h
 * @brief Linux Kernel Lock Analysis
 *
 * This file provides analysis for kernel lock primitives including
 * spinlocks, mutexes, semaphores, and read-write locks.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_LOCK_ANALYSIS_H
#define LINUX_KERNEL_LOCK_ANALYSIS_H

#include "Analysis/Concurrency/LinuxKernel/LinuxKernelOperation.h"
#include "Analysis/Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kernel {

class LinuxKernelProcessModel;

class LinuxKernelLockAnalysis {
public:
  explicit LinuxKernelLockAnalysis(const LinuxKernelProcessModel &model)
      : process_model_(model) {}

  void analyzeLocks();

  struct LockRegion {
    const llvm::Instruction *acquire_inst;
    const llvm::Instruction *release_inst;
    LockID lock;
    LockKind kind;
    bool is_critical;
  };

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findPotentialDeadlocks() const;

  std::vector<const llvm::Instruction *> findDoubleLocks() const;

  std::vector<const llvm::Instruction *> findUnlockWithoutLock() const;

  std::vector<const llvm::Instruction *> findLockWithoutUnlock() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findLockOrderInversions() const;

  std::vector<const llvm::Instruction *> findMixRawAndCookedLocks() const;

  std::vector<const llvm::Instruction *> findSleepInSpinlock() const;

  std::vector<const llvm::Instruction *> findIrqSaveRestoreIssues() const;

  std::vector<LockRegion> getLockRegions() const { return lock_regions_; }

  const std::unordered_map<std::string, size_t> &getLockDiagnostics() const {
    return lock_diagnostics_;
  }

private:
  const LinuxKernelProcessModel &process_model_;
  std::vector<LockRegion> lock_regions_;
  mutable std::unordered_map<std::string, size_t> lock_diagnostics_;

  bool isLockAcquire(OperationKind kind) const;
  bool isLockRelease(OperationKind kind) const;

  bool formsDeadlock(const LockRegion &r1, const LockRegion &r2) const;
  std::vector<LockID> getLockChain(const llvm::Instruction *inst) const;
};

} // namespace kernel

#endif // LINUX_KERNEL_LOCK_ANALYSIS_H
