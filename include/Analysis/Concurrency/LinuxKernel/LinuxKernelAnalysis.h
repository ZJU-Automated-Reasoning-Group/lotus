/**
 * @file LinuxKernelAnalysis.h
 * @brief Linux Kernel Concurrency Analysis
 *
 * This file is the main entry point for Linux Kernel concurrency analysis.
 * It coordinates all kernel-specific analyses including lock analysis,
 * RCU analysis, wait queue analysis, and more.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_ANALYSIS_H
#define LINUX_KERNEL_ANALYSIS_H

#include "Analysis/Concurrency/LinuxKernel/LinuxKernelLockAnalysis.h"
#include "Analysis/Concurrency/LinuxKernel/LinuxKernelOperation.h"
#include "Analysis/Concurrency/LinuxKernel/LinuxKernelProcessModel.h"
#include "Analysis/Concurrency/LinuxKernel/LinuxKernelRCUAnalysis.h"
#include "Analysis/Concurrency/LinuxKernel/LinuxKernelWaitAnalysis.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Module.h>

namespace kernel {

class LinuxKernelAnalysis {
public:
  explicit LinuxKernelAnalysis(llvm::Module &M)
      : module_(M), process_model_(M), lock_analysis_(process_model_),
        rcu_analysis_(process_model_), wait_analysis_(process_model_) {}

  void runAnalysis();

  void printResults(llvm::raw_ostream &OS) const;

  struct AnalysisResults {
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        lock_deadlocks;
    std::vector<const llvm::Instruction *> double_locks;
    std::vector<const llvm::Instruction *> unlock_without_lock;
    std::vector<const llvm::Instruction *> lock_without_unlock;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        lock_order_inversions;
    std::vector<const llvm::Instruction *> sleep_in_atomic;
    std::vector<const llvm::Instruction *> mix_raw_and_cooked;
    std::vector<const llvm::Instruction *> irq_save_restore_issues;

    std::vector<const llvm::Instruction *> rcu_without_grace_period;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        rcu_conflicts;
    std::vector<const llvm::Instruction *> rcu_double_free;
    std::vector<const llvm::Instruction *> deref_after_free;

    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        missing_wake_ups;
    std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
        spurious_wake_ups;
    std::vector<const llvm::Instruction *> missing_completion;
    std::vector<const llvm::Instruction *> double_completion;
    std::vector<const llvm::Instruction *> timer_not_deleted;
    std::vector<const llvm::Instruction *> timer_use_after_delete;

    std::vector<const llvm::Instruction *> use_after_free;
    std::vector<const llvm::Instruction *> timer_issues;
  };

  const AnalysisResults &getResults() const { return results_; }

  size_t getOperationCount(OperationKind kind) const;
  size_t getLockCount() const;
  size_t getRCUSectionCount() const;
  size_t getWaitQueueCount() const;

  const LinuxKernelProcessModel &getProcessModel() const {
    return process_model_;
  }
  const LinuxKernelLockAnalysis &getLockAnalysis() const {
    return lock_analysis_;
  }
  const LinuxKernelRCUAnalysis &getRCUAnalysis() const { return rcu_analysis_; }
  const LinuxKernelWaitAnalysis &getWaitAnalysis() const {
    return wait_analysis_;
  }

private:
  llvm::Module &module_;

  LinuxKernelProcessModel process_model_;
  LinuxKernelLockAnalysis lock_analysis_;
  LinuxKernelRCUAnalysis rcu_analysis_;
  LinuxKernelWaitAnalysis wait_analysis_;

  AnalysisResults results_;
};

} // namespace kernel

#endif // LINUX_KERNEL_ANALYSIS_H
