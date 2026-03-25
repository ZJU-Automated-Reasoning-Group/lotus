/**
 * @file LinuxKernelWaitAnalysis.h
 * @brief Linux Kernel Wait/Notification Analysis
 *
 * This file provides analysis for wait queues, completion variables,
 * timers, and other kernel synchronization mechanisms.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_WAIT_ANALYSIS_H
#define LINUX_KERNEL_WAIT_ANALYSIS_H

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

class LinuxKernelWaitAnalysis {
public:
  explicit LinuxKernelWaitAnalysis(const LinuxKernelProcessModel &model)
      : process_model_(model) {}

  void analyzeWaits();

  struct WaitContext {
    const llvm::Instruction *wait_inst;
    const llvm::Value *wait_queue;
    bool interruptible;
    bool has_timeout;
    const llvm::Instruction *wake_inst;
  };

  struct CompletionContext {
    const llvm::Instruction *init_inst;
    std::vector<const llvm::Instruction *> waiters;
    std::vector<const llvm::Instruction *> completers;
    bool is_done;
  };

  struct TimerContext {
    const llvm::Instruction *setup_inst;
    const llvm::Instruction *mod_inst;
    const llvm::Instruction *delete_inst;
    int delay_ms;
  };

  std::vector<WaitContext> getWaitContexts() const { return wait_contexts_; }

  std::map<WaitQueueID, CompletionContext> getCompletionMap() const {
    return completion_map_;
  }

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findMissingWakeUps() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findSpuriousWakeUps() const;

  std::vector<const llvm::Instruction *> findMissingCompletion() const;

  std::vector<const llvm::Instruction *> findDoubleCompletion() const;

  std::vector<const llvm::Instruction *> findTimerNotDeleted() const;

  std::vector<const llvm::Instruction *> findTimerUseAfterDelete() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findRaceBetweenWaitAndWake() const;

  const std::unordered_map<std::string, size_t> &getWaitDiagnostics() const {
    return wait_diagnostics_;
  }

private:
  const LinuxKernelProcessModel &process_model_;
  std::vector<WaitContext> wait_contexts_;
  std::map<WaitQueueID, CompletionContext> completion_map_;
  std::map<const llvm::Value *, TimerContext> timer_map_;
  mutable std::unordered_map<std::string, size_t> wait_diagnostics_;

  void identifyWaitContexts();
  void identifyCompletions();
  void identifyTimers();

  const WaitQueueEntry *findMatchingWait(WaitQueueID queue) const;
};

} // namespace kernel

#endif // LINUX_KERNEL_WAIT_ANALYSIS_H
