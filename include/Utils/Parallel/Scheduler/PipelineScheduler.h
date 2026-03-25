#ifndef LLVMUTILS_SCHEDULER_PIPELINESCHEDULER_H
#define LLVMUTILS_SCHEDULER_PIPELINESCHEDULER_H

#include "Utils/Parallel/Cancellation.h"
#include "Utils/Parallel/Scheduler/Task.h"
#include "Utils/Parallel/ThreadPool.h"
#include "Utils/Platform/ProgressBar.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

using namespace llvm;

/**
 * PipelineScheduler provides pipeline-style parallel execution of tasks
 * with dependency tracking based on the call graph.
 *
 * Key features:
 * - Bottom-up scheduling based on call graph dependencies
 * - Pipeline pattern: workers execute tasks → master schedules new tasks
 * - Memory management with automatic garbage collection
 * - Progress tracking and status dumping
 *
 * Usage:
 *   PipelineScheduler Scheduler(M, CG);
 *   Scheduler.setTaskCallback([](const Function *F) { ... analyze F ... });
 *   Scheduler.setGCCallback([](const Function *F) { ... release F ... });
 *   Scheduler.run();
 */
class PipelineScheduler {
public:
  /// Analysis type for scheduling strategy
  enum AnalysisType {
    AT_Local,    // Local analysis - all functions can run in parallel
    AT_BottomUp, // Bottom-up analysis - respect call graph dependencies
    AT_TopDown   // Top-down analysis - respect caller dependencies
  };

private:
  /// The module being analyzed
  Module &M;

  /// The call graph for dependency tracking
  CallGraph &CG;

  /// Analysis type determines scheduling strategy
  AnalysisType AType;

  /// Mapping between functions and integers
  /// @{
  std::vector<const Function *> Functions;
  std::map<const Function *, int> FunctionIndexMap;
  std::vector<int> FunctionToSCC;
  /// @}

  struct SCCNode {
    std::vector<int> Members;
    std::vector<int> Callers;
    std::vector<int> Callees;
    unsigned RemainingScheduleDeps = 0;
    unsigned RemainingGCDeps = 0;
    bool Executed = false;
  };

  std::vector<SCCNode> SCCs;

  /// Memory management utilities
  /// @{
  /// Recording the callees of each function
  std::vector<std::set<int>> FunctionCalleeIndexVec;
  /// Recording the functions to release memory
  std::set<const Function *> FunctionToRelease;
  std::atomic<std::uint32_t> PendingReleaseCountForDump{0};
  std::atomic<std::uint32_t> SCCCountForDump{0};
  /// @}

  /// The finished task vector - pipe between workers and master
  /// Workers push finished tasks, master pulls them to schedule new tasks
  /// @{
  std::vector<std::shared_ptr<Task>> FinishedTaskVec;
  std::mutex FTVecMutex;
  std::condition_variable FTVecCond;
  std::atomic<std::uint32_t> OutstandingTaskCount{0};
  std::atomic<std::uint32_t> ActiveTaskCount{0};
  std::atomic<std::uint32_t> FinishedTaskCountForDump{0};
  /// @}

  /// The first task failure observed on a worker thread.
  std::exception_ptr TaskFailure;
  std::mutex FailureMutex;
  std::unique_ptr<ThreadPool::TaskGroup> ExecutionGroup;
  lotus::CancellationSource ExecutionCancellation;

  /// Progress bar for user feedback
  ProgressBar Prog;

  /// Callbacks for client-defined work
  /// @{
  std::function<void(const Function *)> TaskCallback;
  std::function<void(const Function *)> GCCallback;
  void *ClientContext; // Opaque context pointer for callbacks
  /// @}

  /// Configuration options
  /// @{
  int TaskTimeout;      // Timeout for task completion (seconds)
  bool EnableGC;        // Enable automatic garbage collection
  unsigned GCBatchSize; // Number of functions to batch for GC
  /// @}

private:
  /// Execute a task by enqueueing it to the thread pool
  void executeTask(std::shared_ptr<Task> T);

  /// Called when a task is finished
  void finishTask(std::shared_ptr<Task> T);

  /// Record the first task failure for later rethrow on the main thread.
  void recordTaskFailure(std::exception_ptr Failure);

  /// Return the first worker failure, if any.
  std::exception_ptr getTaskFailure();

  /// Reset per-run scheduler state so the scheduler can be reused.
  void resetRunState();

  /// Post-process an SCC task after completion
  std::size_t postProcessSCCFunctionTask(std::shared_ptr<SCCFunctionTask> T);

  /// Wait for tasks and schedule new ones
  void waitTask();

  /// Build the function-level call graph restricted to analyzable functions.
  void buildFunctionGraph();

  /// Compute SCCs from the function-level call graph.
  void computeSCCs();

  /// Build the SCC condensation DAG and initialize scheduler counters.
  void buildSCCDAG();

  /// Return the SCC functions in deterministic execution order.
  std::vector<const Function *> getOrderedSCCFunctions(int SCCIndex) const;

  /// Add an SCC to the pending GC batch and schedule a batch task if needed.
  void maybeReleaseSCC(int SCCIndex, std::size_t &NumGCTasksAdded);

  /// Add a local-analysis function to the pending GC batch.
  void maybeReleaseFunction(const Function *F, std::size_t &NumGCTasksAdded);

  /// Schedule a GC batch for the current release set, if it is non-empty.
  void scheduleGCBatch(std::size_t &NumGCTasksAdded);

public:
  PipelineScheduler(Module &M, CallGraph &CG, AnalysisType AT = AT_BottomUp);
  virtual ~PipelineScheduler();

  /// Set the task callback that will be invoked for each function
  void setTaskCallback(std::function<void(const Function *)> CB) {
    TaskCallback = CB;
  }

  /// Set the garbage collection callback for memory cleanup
  void setGCCallback(std::function<void(const Function *)> CB) {
    GCCallback = CB;
  }

  /// Set opaque context pointer available to callbacks
  void setClientContext(void *Ctx) { ClientContext = Ctx; }

  /// Enable/disable automatic garbage collection
  void setEnableGC(bool Enable) { EnableGC = Enable; }

  /// Set the batch size for garbage collection
  void setGCBatchSize(unsigned Size) { GCBatchSize = Size; }

  /// Set the timeout used to detect stalled scheduling.
  /// Running callbacks are not preempted; they are allowed to finish.
  void setTaskTimeout(int Seconds) { TaskTimeout = Seconds; }

  /// Start scheduling tasks
  void run();

  /// Dump current status (can be called from signal handler)
  void dumpStatus();
};

#endif // LLVMUTILS_SCHEDULER_PIPELINESCHEDULER_H
