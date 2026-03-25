/**
 * @file LinuxKernelRCUAnalysis.h
 * @brief Linux Kernel RCU (Read-Copy-Update) Analysis
 *
 * This file provides analysis for RCU synchronization including
 * read-side critical sections, grace periods, and callback scheduling.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef LINUX_KERNEL_RCU_ANALYSIS_H
#define LINUX_KERNEL_RCU_ANALYSIS_H

#include "Analysis/Concurrency/LinuxKernel/LinuxKernelOperation.h"
#include "Analysis/Concurrency/LinuxKernel/LinuxKernelProcessModel.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace kernel {

class LinuxKernelProcessModel;

class LinuxKernelRCUAnalysis {
public:
  explicit LinuxKernelRCUAnalysis(const LinuxKernelProcessModel &model)
      : process_model_(model) {}

  void analyzeRCU();

  struct RCUCriticalSection {
    const llvm::Instruction *read_lock;
    const llvm::Instruction *read_unlock;
    const llvm::Function *function;

    std::vector<const llvm::Instruction *> protected_accesses;

    bool has_sync = false;
    const llvm::Instruction *sync_point = nullptr;
  };

  struct RCUGracePeriod {
    const llvm::Instruction *sync_inst;
    const llvm::Function *function;
    std::vector<const llvm::Instruction *> callbacks;
  };

  std::vector<RCUCriticalSection> getReadSideSections() const {
    return read_sections_;
  }

  std::vector<RCUGracePeriod> getGracePeriods() const { return grace_periods_; }

  std::vector<const llvm::Instruction *> findReadSideWithoutGracePeriod() const;

  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
  findRCUConflicts() const;

  std::vector<const llvm::Instruction *> findRCUDoubleFree() const;

  std::vector<const llvm::Instruction *> findDerefAfterFree() const;

  std::vector<const llvm::Instruction *> findDerefInWrongSection() const;

  const std::unordered_map<std::string, size_t> &getRCUDiagnostics() const {
    return rcu_diagnostics_;
  }

private:
  const LinuxKernelProcessModel &process_model_;
  std::vector<RCUCriticalSection> read_sections_;
  std::vector<RCUGracePeriod> grace_periods_;
  mutable std::unordered_map<std::string, size_t> rcu_diagnostics_;

  void identifyReadSections();
  void identifyGracePeriods();
  void matchCallbacksToGracePeriods();

  bool isWithinRCUSection(const llvm::Instruction *inst) const;
  const RCUCriticalSection *
  getEnclosingSection(const llvm::Instruction *inst) const;
};

} // namespace kernel

#endif // LINUX_KERNEL_RCU_ANALYSIS_H
