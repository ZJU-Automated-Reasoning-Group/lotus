#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "PipelineScheduler"

using namespace llvm;

static cl::opt<int>
    TaskTimeout("scheduler-task-timeout",
                cl::desc("Timeout for avoiding deadlock (in seconds)"),
                cl::ValueOptional, cl::init(60), cl::ReallyHidden);

namespace {

class SchedulerTimeoutError : public std::runtime_error {
public:
  explicit SchedulerTimeoutError(const std::string &Message)
      : std::runtime_error(Message) {}
};

static_assert(sizeof(std::uint32_t) == sizeof(unsigned int),
              "PipelineScheduler::dumpStatus assumes 32-bit unsigned ints");
static_assert(ATOMIC_INT_LOCK_FREE == 2,
              "PipelineScheduler::dumpStatus requires lock-free counters");

char *appendLiteral(char *Out, const char *End, const char *Text) {
  while (Out != End && *Text != '\0')
    *Out++ = *Text++;
  return Out;
}

char *appendUnsigned(char *Out, const char *End, std::uint32_t Value) {
  char Digits[16];
  unsigned Count = 0;
  do {
    Digits[Count++] = static_cast<char>('0' + (Value % 10));
    Value /= 10;
  } while (Value != 0 && Count < sizeof(Digits));

  while (Count != 0 && Out != End)
    *Out++ = Digits[--Count];
  return Out;
}

void writeStatusLine(const char *Buffer, std::size_t Length) {
  while (Length != 0) {
#if defined(_WIN32)
    const unsigned Chunk =
        static_cast<unsigned>(std::min<std::size_t>(Length, INT_MAX));
    const int Written = _write(2, Buffer, Chunk);
#else
    const ssize_t Written = ::write(STDERR_FILENO, Buffer, Length);
#endif
    if (Written <= 0)
      return;
    Buffer += static_cast<std::size_t>(Written);
    Length -= static_cast<std::size_t>(Written);
  }
}

} // namespace

static inline bool shouldAnalyzeFunction(const Function *Func) {
  return Func && !Func->isIntrinsic() && !Func->isDeclaration();
}

PipelineScheduler::PipelineScheduler(Module &M, CallGraph &CG, AnalysisType AT)
    : M(M), CG(CG), AType(AT),
      Prog("[Pipeline Scheduler]", ProgressBar::PBS_CharacterStyle),
      ClientContext(nullptr), TaskTimeout(::TaskTimeout.getValue()),
      EnableGC(true), GCBatchSize(100) {
  int FuncIndex = 0;
  for (auto &F : M) {
    if (!shouldAnalyzeFunction(&F))
      continue;
    Functions.push_back(&F);
    FunctionIndexMap[&F] = FuncIndex++;
  }

  FunctionCalleeIndexVec.resize(Functions.size());

  LLVM_DEBUG(dbgs() << "[PipelineScheduler] Total functions: "
                    << Functions.size() << "\n");

  if (AType != AT_Local) {
    buildFunctionGraph();
    computeSCCs();
    buildSCCDAG();
  }

  SCCCountForDump.store(static_cast<std::uint32_t>(SCCs.size()),
                        std::memory_order_relaxed);
}

PipelineScheduler::~PipelineScheduler() = default;

void PipelineScheduler::finishTask(std::shared_ptr<Task> T) {
  LLVM_DEBUG(dbgs() << "[PipelineScheduler] Task " << T->toString()
                    << " finished\n");
  {
    std::unique_lock<std::mutex> Lock(FTVecMutex);
    FinishedTaskVec.push_back(std::move(T));
    FinishedTaskCountForDump.store(
        static_cast<std::uint32_t>(FinishedTaskVec.size()),
        std::memory_order_relaxed);
  }
  OutstandingTaskCount.fetch_sub(1, std::memory_order_relaxed);
  ActiveTaskCount.fetch_sub(1, std::memory_order_relaxed);
  FTVecCond.notify_one();
}

void PipelineScheduler::recordTaskFailure(std::exception_ptr Failure) {
  if (!Failure)
    return;

  std::lock_guard<std::mutex> Lock(FailureMutex);
  if (!TaskFailure)
    TaskFailure = Failure;
}

std::exception_ptr PipelineScheduler::getTaskFailure() {
  std::lock_guard<std::mutex> Lock(FailureMutex);
  return TaskFailure;
}

void PipelineScheduler::resetRunState() {
  {
    std::lock_guard<std::mutex> Lock(FTVecMutex);
    FinishedTaskVec.clear();
  }
  FinishedTaskCountForDump.store(0, std::memory_order_relaxed);
  OutstandingTaskCount.store(0, std::memory_order_relaxed);
  ActiveTaskCount.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> Lock(FailureMutex);
    TaskFailure = nullptr;
  }

  FunctionToRelease.clear();
  PendingReleaseCountForDump.store(0, std::memory_order_relaxed);
  ExecutionGroup.reset();
  ExecutionCancellation = lotus::CancellationSource();
  Prog.reset();

  if (AType != AT_Local)
    buildSCCDAG();

  SCCCountForDump.store(static_cast<std::uint32_t>(SCCs.size()),
                        std::memory_order_relaxed);
}

void PipelineScheduler::run() {
  if (!TaskCallback) {
    errs() << "Error: TaskCallback not set! Call setTaskCallback() before "
              "run().\n";
    return;
  }

  llvm::outs() << "Starting pipeline scheduler...\n";
  resetRunState();
  ExecutionGroup = std::make_unique<ThreadPool::TaskGroup>(
      ThreadPool::get()->makeTaskGroup());

  if (AType == AT_Local) {
    for (const auto *F : Functions) {
      auto FTask =
          std::make_shared<FunctionTask>(F, TaskCallback, ClientContext);
      executeTask(FTask);
    }
  } else {
    for (std::size_t SCCIndex = 0; SCCIndex < SCCs.size(); ++SCCIndex) {
      if (SCCs[SCCIndex].RemainingScheduleDeps != 0)
        continue;
      auto SCCTask = std::make_shared<SCCFunctionTask>(
          static_cast<int>(SCCIndex), getOrderedSCCFunctions(SCCIndex),
          TaskCallback, ClientContext);
      executeTask(SCCTask);
    }
  }

  waitTask();

  if (ExecutionGroup) {
    try {
      ExecutionGroup->wait();
    } catch (...) {
      recordTaskFailure(std::current_exception());
    }
    ExecutionGroup.reset();
  }

  if (std::exception_ptr Failure = getTaskFailure())
    std::rethrow_exception(Failure);

  llvm::outs() << "\nPipeline scheduler completed!\n";
}

void PipelineScheduler::executeTask(std::shared_ptr<Task> T) {
  assert(ExecutionGroup && "scheduler execution group must be initialized");
  const lotus::CancellationToken Token = ExecutionCancellation.token();
  OutstandingTaskCount.fetch_add(1, std::memory_order_relaxed);
  try {
    ExecutionGroup->async(Token, [T, this, Token]() {
      ActiveTaskCount.fetch_add(1, std::memory_order_relaxed);
      if (Token.isCancelled()) {
        finishTask(T);
        return;
      }

      try {
        T->run();
      } catch (...) {
        recordTaskFailure(std::current_exception());
        ExecutionCancellation.cancel();
        ThreadPool::get()->cancelPendingTasks();
        finishTask(T);
        throw;
      }
      finishTask(T);
    });
  } catch (...) {
    OutstandingTaskCount.fetch_sub(1, std::memory_order_relaxed);
    throw;
  }
}

void PipelineScheduler::waitTask() {
  const std::size_t NumAllTasks =
      (AType == AT_Local) ? Functions.size() : SCCs.size();
  std::size_t NumUnfinishedTasks = NumAllTasks;
  std::size_t NumGCTasks = 0;
  const auto WaitTimeout = std::chrono::milliseconds(
      TaskTimeout <= 0 ? 1LL : static_cast<long long>(TaskTimeout) * 1000LL);
  std::chrono::steady_clock::time_point QueuedIdleSince;
  bool TrackingQueuedIdle = false;
  auto CancelOutstandingWork = [this]() {
    ExecutionCancellation.cancel();
    ThreadPool::get()->cancelPendingTasks();
  };

  while (NumUnfinishedTasks || NumGCTasks) {
    LLVM_DEBUG(dbgs() << "[PipelineScheduler] Unfinished tasks: "
                      << NumUnfinishedTasks << "\n");

    std::shared_ptr<Task> T;
    {
      std::unique_lock<std::mutex> Lock(FTVecMutex);
      auto RemainingWait = WaitTimeout;
      if (FinishedTaskVec.empty()) {
        const auto Active = ActiveTaskCount.load(std::memory_order_relaxed);
        const auto Outstanding =
            OutstandingTaskCount.load(std::memory_order_relaxed);
        if (Outstanding != 0 && Active == 0) {
          const auto Now = std::chrono::steady_clock::now();
          if (!TrackingQueuedIdle) {
            QueuedIdleSince = Now;
            TrackingQueuedIdle = true;
          } else {
            const auto Elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Now - QueuedIdleSince);
            if (Elapsed >= WaitTimeout) {
              Prog.showProgress(1);
              CancelOutstandingWork();
              recordTaskFailure(std::make_exception_ptr(SchedulerTimeoutError(
                  "PipelineScheduler timed out waiting for queued tasks to "
                  "start")));
              errs() << "\nError: Timeout waiting for queued tasks to start; "
                        "cancelling outstanding work.\n";
              break;
            }
            RemainingWait = WaitTimeout - Elapsed;
          }
        } else if (Active != 0) {
          TrackingQueuedIdle = false;
        }
      }

      FTVecCond.wait_for(Lock, RemainingWait,
                         [this] { return !FinishedTaskVec.empty(); });

      if (FinishedTaskVec.empty()) {
        const auto Active = ActiveTaskCount.load(std::memory_order_relaxed);
        const auto Outstanding =
            OutstandingTaskCount.load(std::memory_order_relaxed);
        if (Active != 0) {
          TrackingQueuedIdle = false;
          continue;
        }
        if (Outstanding != 0)
          continue;
        Prog.showProgress(1);
        CancelOutstandingWork();
        recordTaskFailure(std::make_exception_ptr(SchedulerTimeoutError(
            "PipelineScheduler timed out waiting for tasks")));
        errs() << "\nError: Timeout waiting for tasks; cancelling "
                  "outstanding work.\n";
        break;
      }

      T = FinishedTaskVec.back();
      FinishedTaskVec.pop_back();
      FinishedTaskCountForDump.store(
          static_cast<std::uint32_t>(FinishedTaskVec.size()),
          std::memory_order_relaxed);
      TrackingQueuedIdle = false;
    }

    if (isa<GCTask>(T.get())) {
      assert(NumGCTasks != 0 && "GC task accounting underflow");
      --NumGCTasks;
      if (getTaskFailure()) {
        CancelOutstandingWork();
        break;
      }
      continue;
    }

    assert(NumUnfinishedTasks != 0 && "task accounting underflow");
    --NumUnfinishedTasks;

    if (getTaskFailure()) {
      CancelOutstandingWork();
      break;
    }

    if (AType == AT_Local) {
      if (auto *FTask = dyn_cast<FunctionTask>(T.get()))
        maybeReleaseFunction(FTask->getFunction(), NumGCTasks);
    } else if (auto *SCCTask = dyn_cast<SCCFunctionTask>(T.get())) {
      NumGCTasks += postProcessSCCFunctionTask(
          std::static_pointer_cast<SCCFunctionTask>(T));
    }

    if (getTaskFailure()) {
      CancelOutstandingWork();
      break;
    }

    if (NumAllTasks != 0)
      Prog.showProgress(static_cast<float>(NumAllTasks - NumUnfinishedTasks) /
                        static_cast<float>(NumAllTasks));
    else
      Prog.showProgress(1);
  }

  if (getTaskFailure())
    CancelOutstandingWork();

  if (!getTaskFailure() && EnableGC && GCCallback &&
      !FunctionToRelease.empty()) {
    GCTask TrailingGC(FunctionToRelease, GCCallback, ClientContext);
    TrailingGC.run();
    FunctionToRelease.clear();
    PendingReleaseCountForDump.store(0, std::memory_order_relaxed);
  }

  llvm::outs() << "\n";
}

std::size_t PipelineScheduler::postProcessSCCFunctionTask(
    std::shared_ptr<SCCFunctionTask> T) {
  const int SCCIndex = T->getSCCIndex();
  auto &Current = SCCs[static_cast<std::size_t>(SCCIndex)];
  std::size_t NumGCTasksAdded = 0;

  Current.Executed = true;

  if (AType == AT_BottomUp) {
    for (int CallerSCC : Current.Callers) {
      auto &Caller = SCCs[CallerSCC];
      assert(Caller.RemainingScheduleDeps != 0 &&
             "bottom-up dependency underflow");
      --Caller.RemainingScheduleDeps;
      if (Caller.RemainingScheduleDeps == 0) {
        auto ReadyTask = std::make_shared<SCCFunctionTask>(
            CallerSCC, getOrderedSCCFunctions(CallerSCC), TaskCallback,
            ClientContext);
        executeTask(ReadyTask);
      }
    }
  } else if (AType == AT_TopDown) {
    for (int CalleeSCC : Current.Callees) {
      auto &Callee = SCCs[CalleeSCC];
      assert(Callee.RemainingScheduleDeps != 0 &&
             "top-down dependency underflow");
      --Callee.RemainingScheduleDeps;
      if (Callee.RemainingScheduleDeps == 0) {
        auto ReadyTask = std::make_shared<SCCFunctionTask>(
            CalleeSCC, getOrderedSCCFunctions(CalleeSCC), TaskCallback,
            ClientContext);
        executeTask(ReadyTask);
      }
    }
  }

  if (EnableGC && GCCallback) {
    if (Current.RemainingGCDeps == 0)
      maybeReleaseSCC(SCCIndex, NumGCTasksAdded);

    if (AType == AT_BottomUp) {
      for (int CalleeSCC : Current.Callees) {
        auto &Callee = SCCs[CalleeSCC];
        assert(Callee.RemainingGCDeps != 0 && "GC dependency underflow");
        --Callee.RemainingGCDeps;
        if (Callee.RemainingGCDeps == 0 && Callee.Executed)
          maybeReleaseSCC(CalleeSCC, NumGCTasksAdded);
      }
    } else if (AType == AT_TopDown) {
      for (int CallerSCC : Current.Callers) {
        auto &Caller = SCCs[CallerSCC];
        assert(Caller.RemainingGCDeps != 0 && "GC dependency underflow");
        --Caller.RemainingGCDeps;
        if (Caller.RemainingGCDeps == 0 && Caller.Executed)
          maybeReleaseSCC(CallerSCC, NumGCTasksAdded);
      }
    }
  }

  return NumGCTasksAdded;
}

void PipelineScheduler::buildFunctionGraph() {
  for (std::size_t CallerIndex = 0; CallerIndex < Functions.size();
       ++CallerIndex) {
    const Function *Caller = Functions[CallerIndex];
    CallGraphNode *CallerNode = CG[const_cast<Function *>(Caller)];
    if (!CallerNode)
      continue;

    auto &Callees = FunctionCalleeIndexVec[CallerIndex];
    for (auto &CallRecord : *CallerNode) {
      Function *Callee = CallRecord.second->getFunction();
      if (!shouldAnalyzeFunction(Callee))
        continue;

      auto CalleeIt = FunctionIndexMap.find(Callee);
      if (CalleeIt == FunctionIndexMap.end())
        continue;

      Callees.insert(CalleeIt->second);
    }
  }
}

void PipelineScheduler::computeSCCs() {
  SCCs.clear();
  FunctionToSCC.assign(Functions.size(), -1);

  std::vector<int> Indices(Functions.size(), -1);
  std::vector<int> LowLinks(Functions.size(), -1);
  std::vector<int> Stack;
  std::vector<bool> OnStack(Functions.size(), false);
  int NextIndex = 0;

  auto FunctionOrder = [this](int LHS, int RHS) {
    const Function *LF = Functions[static_cast<std::size_t>(LHS)];
    const Function *RF = Functions[static_cast<std::size_t>(RHS)];
    if (LF->hasName() != RF->hasName())
      return LF->hasName() && !RF->hasName();
    if (LF->hasName() && RF->hasName() && LF->getName() != RF->getName()) {
      return LF->getName() < RF->getName();
    }
    return FunctionIndexMap.at(LF) < FunctionIndexMap.at(RF);
  };

  std::function<void(int)> StrongConnect = [&](int V) {
    Indices[V] = NextIndex;
    LowLinks[V] = NextIndex;
    ++NextIndex;
    Stack.push_back(V);
    OnStack[V] = true;

    for (int W : FunctionCalleeIndexVec[static_cast<std::size_t>(V)]) {
      if (Indices[W] == -1) {
        StrongConnect(W);
        LowLinks[V] = std::min(LowLinks[V], LowLinks[W]);
      } else if (OnStack[W]) {
        LowLinks[V] = std::min(LowLinks[V], Indices[W]);
      }
    }

    if (LowLinks[V] != Indices[V])
      return;

    SCCNode Node;
    const int SCCIndex = static_cast<int>(SCCs.size());
    while (true) {
      int W = Stack.back();
      Stack.pop_back();
      OnStack[W] = false;
      Node.Members.push_back(W);
      FunctionToSCC[static_cast<std::size_t>(W)] = SCCIndex;
      if (W == V)
        break;
    }

    std::sort(Node.Members.begin(), Node.Members.end(), FunctionOrder);
    SCCs.push_back(std::move(Node));
  };

  for (std::size_t FunctionIndex = 0; FunctionIndex < Functions.size();
       ++FunctionIndex) {
    if (Indices[FunctionIndex] == -1)
      StrongConnect(static_cast<int>(FunctionIndex));
  }
}

void PipelineScheduler::buildSCCDAG() {
  std::vector<std::set<int>> SCCCallers(SCCs.size());
  std::vector<std::set<int>> SCCCallees(SCCs.size());

  for (std::size_t CallerIndex = 0; CallerIndex < Functions.size();
       ++CallerIndex) {
    const int CallerSCC = FunctionToSCC[CallerIndex];
    for (int CalleeIndex : FunctionCalleeIndexVec[CallerIndex]) {
      const int CalleeSCC =
          FunctionToSCC[static_cast<std::size_t>(CalleeIndex)];
      if (CallerSCC == CalleeSCC)
        continue;
      SCCCallees[CallerSCC].insert(CalleeSCC);
      SCCCallers[CalleeSCC].insert(CallerSCC);
    }
  }

  for (std::size_t SCCIndex = 0; SCCIndex < SCCs.size(); ++SCCIndex) {
    auto &Node = SCCs[SCCIndex];
    Node.Callers.assign(SCCCallers[SCCIndex].begin(),
                        SCCCallers[SCCIndex].end());
    Node.Callees.assign(SCCCallees[SCCIndex].begin(),
                        SCCCallees[SCCIndex].end());
    Node.RemainingGCDeps =
        (AType == AT_BottomUp) ? Node.Callers.size() : Node.Callees.size();
    Node.RemainingScheduleDeps =
        (AType == AT_BottomUp) ? Node.Callees.size() : Node.Callers.size();
    Node.Executed = false;
  }
}

std::vector<const Function *>
PipelineScheduler::getOrderedSCCFunctions(int SCCIndex) const {
  std::vector<const Function *> OrderedFunctions;
  for (int FunctionIndex : SCCs[static_cast<std::size_t>(SCCIndex)].Members)
    OrderedFunctions.push_back(
        Functions[static_cast<std::size_t>(FunctionIndex)]);
  return OrderedFunctions;
}

void PipelineScheduler::maybeReleaseSCC(int SCCIndex,
                                        std::size_t &NumGCTasksAdded) {
  for (int FunctionIndex : SCCs[static_cast<std::size_t>(SCCIndex)].Members)
    FunctionToRelease.insert(
        Functions[static_cast<std::size_t>(FunctionIndex)]);

  PendingReleaseCountForDump.store(
      static_cast<std::uint32_t>(FunctionToRelease.size()),
      std::memory_order_relaxed);
  if (FunctionToRelease.size() >= GCBatchSize)
    scheduleGCBatch(NumGCTasksAdded);
}

void PipelineScheduler::maybeReleaseFunction(
    const Function *F, std::size_t &NumGCTasksAdded) {
  FunctionToRelease.insert(F);
  PendingReleaseCountForDump.store(
      static_cast<std::uint32_t>(FunctionToRelease.size()),
      std::memory_order_relaxed);
  if (FunctionToRelease.size() >= GCBatchSize)
    scheduleGCBatch(NumGCTasksAdded);
}

void PipelineScheduler::scheduleGCBatch(std::size_t &NumGCTasksAdded) {
  if (FunctionToRelease.empty())
    return;

  auto GTask =
      std::make_shared<GCTask>(FunctionToRelease, GCCallback, ClientContext);
  executeTask(GTask);
  FunctionToRelease.clear();
  PendingReleaseCountForDump.store(0, std::memory_order_relaxed);
  ++NumGCTasksAdded;
}

void PipelineScheduler::dumpStatus() {
  char Buffer[192];
  char *Out = Buffer;
  const char *End = Buffer + sizeof(Buffer);

  Out = appendLiteral(Out, End, "\n[PipelineScheduler Status]\n");
  Out = appendLiteral(Out, End, "  Finished tasks in queue: ");
  Out = appendUnsigned(
      Out, End,
      FinishedTaskCountForDump.load(std::memory_order_relaxed));
  Out = appendLiteral(Out, End, "\n  Functions to release: ");
  Out = appendUnsigned(
      Out, End,
      PendingReleaseCountForDump.load(std::memory_order_relaxed));
  Out = appendLiteral(Out, End, "\n  SCCs tracked: ");
  Out = appendUnsigned(Out, End,
                       SCCCountForDump.load(std::memory_order_relaxed));
  Out = appendLiteral(Out, End, "\n");

  writeStatusLine(Buffer, static_cast<std::size_t>(Out - Buffer));
}
