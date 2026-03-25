/**
 * \file ThreadPool.h
 * \brief Multi-threaded task scheduling utility
 * \author Lotus Team
 *
 * This file provides a thread pool implementation for parallel task execution.
 * The thread pool manages a set of worker threads that process tasks from
 * a shared queue, enabling efficient parallelization of work items.
 *
 * Features:
 * - Dynamic task enqueueing
 * - Thread-local storage support
 * - Wait synchronization for task completion
 * - Configurable number of worker threads
 */
#ifndef SUPPORT_THREADPOOL_H
#define SUPPORT_THREADPOOL_H

#include "Utils/ADT/MapIterators.h"
#include "Utils/Parallel/Cancellation.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/ManagedStatic.h>

namespace lotus {
namespace detail {

template <class ReturnTy>
inline typename std::enable_if<std::is_void<ReturnTy>::value, void>::type
cancelledReturnValue() {}

template <class ReturnTy>
inline typename std::enable_if<!std::is_void<ReturnTy>::value, ReturnTy>::type
cancelledReturnValue() {
  throw TaskCancelledError();
}

} // namespace detail
} // namespace lotus

class ThreadPool {
private:
  ThreadPool();
  bool runOnePendingTaskOrWait();

public:
  class TaskGroup {
  private:
    struct SharedState {
      std::mutex Mutex;
      std::condition_variable Condition;
      std::size_t PendingTasks = 0;
      std::exception_ptr Failure;
      bool FailureObserved = false;
    };

    ThreadPool *Owner;
    std::shared_ptr<SharedState> GroupState;

    explicit TaskGroup(ThreadPool *Pool)
        : Owner(Pool), GroupState(std::make_shared<SharedState>()) {}

    friend class ThreadPool;

  public:
    TaskGroup() : Owner(nullptr), GroupState(nullptr) {}
    TaskGroup(const TaskGroup &) = delete;
    TaskGroup &operator=(const TaskGroup &) = delete;
    TaskGroup(TaskGroup &&) = default;
    TaskGroup &operator=(TaskGroup &&) = default;

    ~TaskGroup() {
      if (!GroupState)
        return;

      std::unique_lock<std::mutex> Lock(GroupState->Mutex);
      assert(GroupState->PendingTasks == 0 &&
             "TaskGroup destroyed with pending tasks; call wait()");
      assert((!GroupState->Failure || GroupState->FailureObserved) &&
             "TaskGroup destroyed with unobserved task failure; call wait()");
    }

    template <class F, class... Args>
    auto async(F &&Func, Args &&...Arguments)
        -> std::future<decltype(std::declval<F>()(std::declval<Args>()...))>;

    template <class F, class... Args>
    auto async(const lotus::CancellationToken &Token, F &&Func,
               Args &&...Arguments)
        -> std::future<decltype(std::declval<F>()(std::declval<Args>()...))>;

    void wait() {
      assert(GroupState && "waiting on a moved-from TaskGroup");

      while (true) {
        std::exception_ptr Failure;
        {
          std::unique_lock<std::mutex> Lock(GroupState->Mutex);
          if (GroupState->PendingTasks == 0) {
            Failure = GroupState->Failure;
            GroupState->FailureObserved = true;
            if (Failure)
              std::rethrow_exception(Failure);
            return;
          }
        }

        if (Owner && Owner->hasWorkers() && Owner->isWorkerThread()) {
          if (!Owner->runOnePendingTaskOrWait()) {
            std::unique_lock<std::mutex> Lock(GroupState->Mutex);
            if (GroupState->PendingTasks != 0)
              GroupState->Condition.wait_for(Lock,
                                             std::chrono::milliseconds(1));
          }
          continue;
        }

        std::unique_lock<std::mutex> Lock(GroupState->Mutex);
        GroupState->Condition.wait(
            Lock, [this] { return GroupState->PendingTasks == 0; });
        Failure = GroupState->Failure;
        GroupState->FailureObserved = true;
        if (Failure)
          std::rethrow_exception(Failure);
        return;
      }
    }
  };

  template <class T> class ThreadLocalSlot {
  private:
    struct SharedState {
      std::function<T()> Factory;
      mutable std::mutex Mutex;
      std::map<std::thread::id, std::unique_ptr<T>> Values;

      explicit SharedState(std::function<T()> F) : Factory(std::move(F)) {}

      T &getForCurrentThread() {
        std::lock_guard<std::mutex> Lock(Mutex);
        auto Id = std::this_thread::get_id();
        auto &Value = Values[Id];
        if (!Value)
          Value = std::make_unique<T>(Factory());
        return *Value;
      }
    };

    std::shared_ptr<SharedState> SlotState;

    explicit ThreadLocalSlot(std::shared_ptr<SharedState> State)
        : SlotState(std::move(State)) {}

    friend class ThreadPool;

  public:
    ThreadLocalSlot() = default;

    T &get() const {
      assert(SlotState && "accessing a moved-from ThreadLocalSlot");
      return SlotState->getForCurrentThread();
    }

    template <class Visitor> void forEachValue(Visitor &&Visit) const {
      assert(SlotState && "accessing a moved-from ThreadLocalSlot");
      std::vector<const T *> Snapshot;
      {
        std::lock_guard<std::mutex> Lock(SlotState->Mutex);
        Snapshot.reserve(SlotState->Values.size());
        for (const auto &Entry : SlotState->Values) {
          if (Entry.second)
            Snapshot.push_back(Entry.second.get());
        }
      }
      for (const T *Value : Snapshot)
        Visit(*Value);
    }
  };

  template <class T, class MergeFn> class ThreadLocalReducer {
  private:
    ThreadLocalSlot<T> Slot;
    MergeFn Merge;

  public:
    ThreadLocalReducer(ThreadLocalSlot<T> ReducerSlot, MergeFn MergeFnValue)
        : Slot(std::move(ReducerSlot)), Merge(std::move(MergeFnValue)) {}

    T &local() { return Slot.get(); }

    const T &local() const { return Slot.get(); }

    T reduce(T Init) const {
      Slot.forEachValue(
          [&](const T &Value) { Init = Merge(std::move(Init), Value); });
      return Init;
    }
  };

  ~ThreadPool();

  unsigned workerCount() const { return Workers.size(); }

  bool hasWorkers() const { return workerCount() != 0; }

  bool isWorkerThread() const {
    return WorkerIds.find(std::this_thread::get_id()) != WorkerIds.end();
  }

  TaskGroup makeTaskGroup() { return TaskGroup(this); }

  /// add new work item to the pool
  template <class F, class... Args>
  auto enqueue(F &&, Args &&...)
      -> std::future<decltype(std::declval<F>()(std::declval<Args>()...))>;

  template <class T, class Factory>
  ThreadLocalSlot<T> makeThreadLocal(Factory &&FactoryFn);

  template <class T> ThreadLocalSlot<T> makeThreadLocal();

  template <class T, class Factory, class MergeFn>
  ThreadLocalReducer<T, typename std::decay<MergeFn>::type>
  makeThreadLocalReducer(Factory &&FactoryFn, MergeFn &&MergeFnValue);

  template <class T, class MergeFn>
  ThreadLocalReducer<T, typename std::decay<MergeFn>::type>
  makeThreadLocalReducer(MergeFn &&MergeFnValue);

  template <class Index, class Body>
  void parallelFor(Index Begin, Index End, std::size_t GrainSize, Body &&Fn);

  template <class Index, class Body>
  void parallelFor(Index Begin, Index End, std::size_t GrainSize,
                   const lotus::CancellationToken &Token, Body &&Fn);

  template <class Range, class Body>
  void parallelForEach(Range &&R, std::size_t GrainSize, Body &&Fn);

  template <class Range, class Body>
  void parallelForEach(Range &&R, std::size_t GrainSize,
                       const lotus::CancellationToken &Token, Body &&Fn);

  template <class Index, class Accumulator, class MapFn, class ReduceFn>
  Accumulator parallelReduce(Index Begin, Index End, std::size_t GrainSize,
                             Accumulator Init, MapFn &&Map, ReduceFn &&Reduce);

  /// Wait until no tasks remain
  void wait();

  /// Execute and remove any queued tasks whose cancellation predicates now
  /// report that they can be completed without a worker thread.
  void cancelPendingTasks();

  /// Legacy raw thread-local support. New code should prefer makeThreadLocal()
  /// and makeThreadLocalReducer().
  template <class LocalTy> void initThreadLocal() {
    // Add main thread id
    auto Id = std::this_thread::get_id();
    if (ThreadLocals.find(Id) == ThreadLocals.end()) {
      ThreadLocals[Id] = new LocalTy;
    }

    for (auto &Worker : Workers) {
      if (ThreadLocals[Worker.get_id()]) {
        llvm_unreachable("thread local already declared");
      }
      ThreadLocals[Worker.get_id()] = new LocalTy;
    }
  }

  template <class LocalTy> void deinitThreadLocal() {
    for (auto &It : ThreadLocals) {
      delete (LocalTy *)It.second;
      It.second = nullptr;
    }
  }

  template <class LocalTy> LocalTy *getThreadLocal() const {
    auto It = ThreadLocals.find(std::this_thread::get_id());
    assert(It != ThreadLocals.end());
    return (LocalTy *)It->second;
  }

  value_iterator<std::map<std::thread::id, void *>::iterator>
  threadLocalsBegin() {
    return {ThreadLocals.begin()};
  }

  value_iterator<std::map<std::thread::id, void *>::iterator>
  threadLocalsEnd() {
    return {ThreadLocals.end()};
  }

private:
  struct PendingTask {
    std::function<void()> Run;
    std::function<bool()> TryCancel;
  };

  void enqueuePendingTask(PendingTask Task);

  /// We need to keep track of threads so we can join them recording the
  /// workers of the thread pool.
  std::vector<std::thread> Workers;

  /// The task queue containing tasks.
  std::deque<PendingTask> TaskQueue;

  std::mutex QueueMutex;             ///< The lock
  std::condition_variable Condition; ///< the wait cond

  bool IsStop;        ///< identifying if the thread pool is running
  int NumRunningTask; /// < number of running task

  std::map<std::thread::id, void *> ThreadLocals;
  std::set<std::thread::id> WorkerIds;

public:
  static ThreadPool *get();
};

template <class F, class... Args>
auto ThreadPool::enqueue(F &&Func, Args &&...Arguments)
    -> std::future<decltype(std::declval<F>()(std::declval<Args>()...))> {
  using return_type = decltype(std::declval<F>()(std::declval<Args>()...));

  auto Task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(Func), std::forward<Args>(Arguments)...));
  std::future<return_type> Res = Task->get_future();

  if (!hasWorkers()) {
    (*Task)();
    return Res;
  }

  enqueuePendingTask(PendingTask{[Task]() { (*Task)(); }, {}});
  return Res;
}

template <class F, class... Args>
auto ThreadPool::TaskGroup::async(F &&Func, Args &&...Arguments)
    -> std::future<decltype(std::declval<F>()(std::declval<Args>()...))> {
  using return_type = decltype(std::declval<F>()(std::declval<Args>()...));

  assert(Owner && GroupState && "scheduling work on a moved-from TaskGroup");

  auto State = GroupState;
  auto BoundTask =
      std::bind(std::forward<F>(Func), std::forward<Args>(Arguments)...);
  auto Task = std::make_shared<std::packaged_task<return_type()>>(
      [State, BoundTask]() mutable -> return_type {
        try {
          return BoundTask();
        } catch (...) {
          std::lock_guard<std::mutex> Lock(State->Mutex);
          if (!State->Failure)
            State->Failure = std::current_exception();
          throw;
        }
      });

  std::future<return_type> Res = Task->get_future();
  {
    std::lock_guard<std::mutex> Lock(State->Mutex);
    ++State->PendingTasks;
  }

  try {
    auto CompleteTask = [State, Task]() mutable {
      (*Task)();

      std::lock_guard<std::mutex> Lock(State->Mutex);
      assert(State->PendingTasks != 0 && "TaskGroup pending count underflow");
      --State->PendingTasks;
      State->Condition.notify_all();
    };
    Owner->enqueuePendingTask(PendingTask{CompleteTask, {}});
  } catch (...) {
    std::lock_guard<std::mutex> Lock(State->Mutex);
    assert(State->PendingTasks != 0 && "TaskGroup pending count underflow");
    --State->PendingTasks;
    State->Condition.notify_all();
    throw;
  }
  return Res;
}

template <class F, class... Args>
auto ThreadPool::TaskGroup::async(const lotus::CancellationToken &Token,
                                  F &&Func, Args &&...Arguments)
    -> std::future<decltype(std::declval<F>()(std::declval<Args>()...))> {
  using return_type = decltype(std::declval<F>()(std::declval<Args>()...));

  static_assert(std::is_void<return_type>::value ||
                    std::is_default_constructible<return_type>::value,
                "cancellable async requires void or default-constructible "
                "return types");

  assert(Owner && GroupState && "scheduling work on a moved-from TaskGroup");

  auto State = GroupState;
  auto BoundTask =
      std::bind(std::forward<F>(Func), std::forward<Args>(Arguments)...);
  auto Task = std::make_shared<std::packaged_task<return_type()>>(
      [State, BoundTask, Token]() mutable -> return_type {
        try {
          if (Token.isCancelled())
            return lotus::detail::cancelledReturnValue<return_type>();
          return BoundTask();
        } catch (...) {
          std::lock_guard<std::mutex> Lock(State->Mutex);
          if (!State->Failure)
            State->Failure = std::current_exception();
          throw;
        }
      });

  std::future<return_type> Res = Task->get_future();
  {
    std::lock_guard<std::mutex> Lock(State->Mutex);
    ++State->PendingTasks;
  }

  try {
    auto CompleteTask = [State, Task]() mutable {
      (*Task)();

      std::lock_guard<std::mutex> Lock(State->Mutex);
      assert(State->PendingTasks != 0 && "TaskGroup pending count underflow");
      --State->PendingTasks;
      State->Condition.notify_all();
    };
    Owner->enqueuePendingTask(PendingTask{
        CompleteTask,
        [Token, CompleteTask]() mutable {
          if (!Token.isCancelled())
            return false;
          CompleteTask();
          return true;
        }});
  } catch (...) {
    std::lock_guard<std::mutex> Lock(State->Mutex);
    assert(State->PendingTasks != 0 && "TaskGroup pending count underflow");
    --State->PendingTasks;
    State->Condition.notify_all();
    throw;
  }
  return Res;
}

template <class T, class Factory>
auto ThreadPool::makeThreadLocal(Factory &&FactoryFn) -> ThreadLocalSlot<T> {
  using SlotState = typename ThreadPool::ThreadLocalSlot<T>::SharedState;
  auto State = std::make_shared<SlotState>(
      std::function<T()>(std::forward<Factory>(FactoryFn)));
  ThreadLocalSlot<T> Slot(State);
  (void)Slot.get();
  return Slot;
}

template <class T> auto ThreadPool::makeThreadLocal() -> ThreadLocalSlot<T> {
  return makeThreadLocal<T>([]() { return T(); });
}

template <class T, class Factory, class MergeFn>
auto ThreadPool::makeThreadLocalReducer(Factory &&FactoryFn,
                                        MergeFn &&MergeFnValue)
    -> ThreadLocalReducer<T, typename std::decay<MergeFn>::type> {
  using ReducerTy = ThreadLocalReducer<T, typename std::decay<MergeFn>::type>;
  return ReducerTy(makeThreadLocal<T>(std::forward<Factory>(FactoryFn)),
                   std::forward<MergeFn>(MergeFnValue));
}

template <class T, class MergeFn>
auto ThreadPool::makeThreadLocalReducer(MergeFn &&MergeFnValue)
    -> ThreadLocalReducer<T, typename std::decay<MergeFn>::type> {
  return makeThreadLocalReducer<T>([]() { return T(); },
                                   std::forward<MergeFn>(MergeFnValue));
}

template <class Index, class Body>
void ThreadPool::parallelFor(Index Begin, Index End, std::size_t GrainSize,
                             Body &&Fn) {
  parallelFor(Begin, End, GrainSize, lotus::CancellationToken(),
              std::forward<Body>(Fn));
}

template <class Index, class Body>
void ThreadPool::parallelFor(Index Begin, Index End, std::size_t GrainSize,
                             const lotus::CancellationToken &Token, Body &&Fn) {
  static_assert(std::is_integral<Index>::value,
                "parallelFor requires an integral index type");

  if (End <= Begin || Token.isCancelled())
    return;

  const std::size_t Total = static_cast<std::size_t>(
      static_cast<long long>(End) - static_cast<long long>(Begin));
  if (GrainSize == 0)
    GrainSize = 1;

  if (!hasWorkers() || Total <= GrainSize) {
    for (Index I = Begin; I < End && !Token.isCancelled(); ++I)
      Fn(I);
    return;
  }

  TaskGroup Group = makeTaskGroup();
  for (Index ChunkBegin = Begin; ChunkBegin < End;) {
    const std::size_t Remaining = static_cast<std::size_t>(
        static_cast<long long>(End) - static_cast<long long>(ChunkBegin));
    const std::size_t ChunkSize = std::min(GrainSize, Remaining);
    const Index ChunkEnd = static_cast<Index>(
        static_cast<long long>(ChunkBegin) + static_cast<long long>(ChunkSize));
    Group.async(Token, [ChunkBegin, ChunkEnd, &Fn, Token]() {
      for (Index I = ChunkBegin; I < ChunkEnd && !Token.isCancelled(); ++I)
        Fn(I);
    });
    ChunkBegin = ChunkEnd;
  }

  Group.wait();
}

template <class Range, class Body>
void ThreadPool::parallelForEach(Range &&R, std::size_t GrainSize, Body &&Fn) {
  parallelForEach(std::forward<Range>(R), GrainSize, lotus::CancellationToken(),
                  std::forward<Body>(Fn));
}

template <class Range, class Body>
void ThreadPool::parallelForEach(Range &&R, std::size_t GrainSize,
                                 const lotus::CancellationToken &Token,
                                 Body &&Fn) {
  auto Begin = std::begin(R);
  auto End = std::end(R);
  using Iterator = decltype(Begin);
  using IteratorCategory =
      typename std::iterator_traits<Iterator>::iterator_category;

  static_assert(std::is_base_of<std::forward_iterator_tag, IteratorCategory>::value,
                "parallelForEach requires at least forward iterators");

  std::vector<Iterator> Elements;
  for (auto It = Begin; It != End; ++It)
    Elements.push_back(It);

  parallelFor<std::size_t>(0, Elements.size(), GrainSize, Token,
                           [&](std::size_t Index) { Fn(*Elements[Index]); });
}

template <class Index, class Accumulator, class MapFn, class ReduceFn>
Accumulator ThreadPool::parallelReduce(Index Begin, Index End,
                                       std::size_t GrainSize, Accumulator Init,
                                       MapFn &&Map, ReduceFn &&Reduce) {
  static_assert(std::is_integral<Index>::value,
                "parallelReduce requires an integral index type");
  using MappedTy = typename std::decay<
      decltype(std::declval<MapFn &>()(std::declval<Index>()))>::type;

  if (End <= Begin)
    return Init;

  const std::size_t Total = static_cast<std::size_t>(
      static_cast<long long>(End) - static_cast<long long>(Begin));
  if (GrainSize == 0)
    GrainSize = 1;

  if (!hasWorkers() || Total <= GrainSize) {
    for (Index I = Begin; I < End; ++I)
      Init = Reduce(std::move(Init), Map(I));
    return Init;
  }

  const std::size_t ChunkCount = (Total + GrainSize - 1) / GrainSize;
  std::vector<std::unique_ptr<std::vector<MappedTy>>> Partials(ChunkCount);
  std::mutex ErrorMutex;
  std::exception_ptr Error;
  TaskGroup Group = makeTaskGroup();
  std::size_t ChunkIndex = 0;
  for (Index ChunkBegin = Begin; ChunkBegin < End; ++ChunkIndex) {
    const std::size_t Remaining = static_cast<std::size_t>(
        static_cast<long long>(End) - static_cast<long long>(ChunkBegin));
    const std::size_t ChunkSize = std::min(GrainSize, Remaining);
    const Index ChunkEnd = static_cast<Index>(
        static_cast<long long>(ChunkBegin) + static_cast<long long>(ChunkSize));
    Group.async([ChunkBegin, ChunkEnd, ChunkIndex, ChunkSize, &Partials, &Error,
                 &ErrorMutex, &Map]() mutable {
      try {
        auto Local = std::make_unique<std::vector<MappedTy>>();
        Local->reserve(ChunkSize);
        for (Index I = ChunkBegin; I < ChunkEnd; ++I)
          Local->emplace_back(Map(I));
        Partials[ChunkIndex] = std::move(Local);
      } catch (...) {
        std::lock_guard<std::mutex> Lock(ErrorMutex);
        if (!Error)
          Error = std::current_exception();
      }
    });
    ChunkBegin = ChunkEnd;
  }

  Group.wait();

  if (Error)
    std::rethrow_exception(Error);

  for (std::size_t PartialIndex = 0; PartialIndex < Partials.size();
       ++PartialIndex) {
    assert(Partials[PartialIndex] && "parallelReduce partial chunk missing");
    for (std::size_t ValueIndex = 0; ValueIndex < Partials[PartialIndex]->size();
         ++ValueIndex) {
      MappedTy Value = std::move((*Partials[PartialIndex])[ValueIndex]);
      Init = Reduce(std::move(Init), std::move(Value));
    }
  }
  return Init;
}

#endif
