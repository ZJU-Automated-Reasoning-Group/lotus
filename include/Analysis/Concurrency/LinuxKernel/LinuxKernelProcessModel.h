/**
 * @file LinuxKernelProcessModel.h
 * @brief Linux Kernel Process/Thread Model
 *
 * This file provides the kernel process model that tracks kernel threads,
 * their operations, and relationships.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_PROCESS_MODEL_H
#define LINUX_KERNEL_PROCESS_MODEL_H

#include "Analysis/Concurrency/LinuxKernel/LinuxKernelOperation.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Module.h>

namespace kernel {

class LinuxKernelLockAnalysis;
class LinuxKernelRCUAnalysis;
class LinuxKernelWaitAnalysis;

class LinuxKernelProcessModel {
public:
  LinuxKernelProcessModel(llvm::Module &M) : module_(M) {}

  void analyzeModule();

  const std::vector<KernelOperation> &getAllOperations() const {
    return all_operations_;
  }

  const std::unordered_map<OperationKind, size_t> &
  getOperationKindCounts() const {
    return operation_kind_counts_;
  }

  const llvm::Module &getModule() const { return module_; }

  std::vector<KernelOperation> getOperationsByKind(OperationKind kind) const;

  std::vector<KernelOperation> getOperationsByLock(LockID lock) const;

  const std::map<LockID, LockInfo> &getLockInfoMap() const {
    return lock_info_map_;
  }

  std::vector<KernelOperation> findLockAcquiresWithoutRelease() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findPotentialDeadlocks() const;

  std::vector<const llvm::Instruction *> findDoubleLocks() const;

  std::vector<const llvm::Instruction *> findUnlockWithoutLock() const;

  std::vector<const llvm::Instruction *> findMixRawAndcooked() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findLockOrderInversion() const;

  std::vector<const llvm::Instruction *> findRCUWithoutGracePeriod() const;

  std::vector<const llvm::Instruction *> findSleepInAtomic() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findUseAfterFree() const;

  std::vector<const llvm::Instruction *> findTimerIssues() const;

  std::vector<const llvm::Instruction *> findIrqSaveRestoreMismatch() const;

private:
  llvm::Module &module_;

  std::vector<KernelOperation> all_operations_;
  std::unordered_map<OperationKind, size_t> operation_kind_counts_;

  std::map<LockID, LockInfo> lock_info_map_;
  std::map<RCUSyncPointID, RCUSection> rcu_sections_;
  std::map<WaitQueueID, WaitQueueEntry> wait_queue_entries_;

  std::map<std::pair<const llvm::Function *, LockID>, int> lock_depth_;

  OperationKind classifyOperation(const llvm::Instruction *inst,
                                  const llvm::StringRef &func_name) const;
  LockKind classifyLockKind(const llvm::StringRef &func_name) const;

  void extractLockDetails(KernelOperation &op);
  void extractRCUDetails(KernelOperation &op);
  void extractWaitQueueDetails(KernelOperation &op);
  void extractTimerDetails(KernelOperation &op);
  void extractAtomicDetails(KernelOperation &op);

  void trackLockState(KernelOperation &op);
  void analyzeLockUsage();

  bool isInAtomicContext(const llvm::Instruction *inst) const;
  bool maySleep(const llvm::Instruction *inst) const;
};

} // namespace kernel

#endif // LINUX_KERNEL_PROCESS_MODEL_H
