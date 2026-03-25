#pragma once

#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Checker/Concurrency/ConcurrencyBugReport.h"

#include <memory>
#include <vector>

namespace concurrency {

class OpenMPChecker {
public:
  OpenMPChecker(llvm::Module &module, OpenMP::OpenMPTaskGraph *task_graph,
                ThreadAPI *thread_api);

  std::vector<ConcurrencyBugReport> checkOpenMPBugs();

private:
  llvm::Module &m_module;
  OpenMP::OpenMPTaskGraph *m_taskGraph;
  std::unique_ptr<OpenMP::OpenMPTaskGraph> m_ownedTaskGraph;
  ThreadAPI *m_threadAPI;

  void ensureTaskGraph();
  std::vector<ConcurrencyBugReport> checkPartialTaskSynchronization() const;
  std::vector<ConcurrencyBugReport> checkTaskgroupStructure() const;
  std::vector<ConcurrencyBugReport> checkAtomicRegionStructure() const;
  std::vector<ConcurrencyBugReport> checkDetachedTaskLeak() const;
  std::vector<ConcurrencyBugReport> checkNestedSingle() const;
  std::vector<ConcurrencyBugReport> checkNowaitMissingBarrier() const;
  std::vector<ConcurrencyBugReport> checkMissingFlush() const;
  std::vector<ConcurrencyBugReport> checkIncorrectNumThreads() const;
  std::vector<ConcurrencyBugReport> checkReductionError() const;
  std::vector<ConcurrencyBugReport> checkTaskwaitMissing() const;
  std::vector<ConcurrencyBugReport> checkNestedParallelDisabled() const;
  std::vector<ConcurrencyBugReport> checkSharedPrivateConflict() const;
  std::vector<ConcurrencyBugReport> checkIfFalseParallel() const;
  std::vector<ConcurrencyBugReport> checkOrderedDependency() const;
  std::vector<ConcurrencyBugReport> checkLastprivateMissing() const;
  std::vector<ConcurrencyBugReport> checkCopyinNotShared() const;
  std::vector<ConcurrencyBugReport> checkBarrierInCritical() const;
  std::vector<ConcurrencyBugReport> checkPrivateInLoop() const;
  std::vector<ConcurrencyBugReport> checkMissingSchedule() const;
};

} // namespace concurrency