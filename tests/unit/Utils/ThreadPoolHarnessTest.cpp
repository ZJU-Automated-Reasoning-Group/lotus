#include "Utils/Parallel/ThreadPool.h"

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <llvm/Support/CommandLine.h>
#include <gtest/gtest.h>

namespace {

struct SlotValue {
  int token = 0;
  std::shared_ptr<std::atomic<int>> destroyed;
  bool counted = false;

  SlotValue() = default;
  SlotValue(int value, std::shared_ptr<std::atomic<int>> destroyed_counter)
      : token(value), destroyed(std::move(destroyed_counter)), counted(true) {}

  SlotValue(const SlotValue &) = delete;
  SlotValue &operator=(const SlotValue &) = delete;
  SlotValue(SlotValue &&other) noexcept
      : token(other.token), destroyed(std::move(other.destroyed)),
        counted(other.counted) {
    other.counted = false;
  }
  SlotValue &operator=(SlotValue &&other) noexcept {
    if (this == &other)
      return *this;
    token = other.token;
    destroyed = std::move(other.destroyed);
    counted = other.counted;
    other.counted = false;
    return *this;
  }

  ~SlotValue() {
    if (counted && destroyed)
      destroyed->fetch_add(1, std::memory_order_relaxed);
  }
};

struct ThrowingBinderTask {
  ThrowingBinderTask() = default;
  ThrowingBinderTask(const ThrowingBinderTask &) {
    throw std::runtime_error("bind copy failed");
  }
  ThrowingBinderTask(ThrowingBinderTask &&) noexcept = default;

  void operator()() const {}
};

TEST(ThreadPoolHarnessTest, WorkerIntrospectionMatchesPoolState) {
  ThreadPool *pool = ThreadPool::get();
  EXPECT_EQ(pool->hasWorkers(), pool->workerCount() != 0u);
}

TEST(ThreadPoolHarnessTest, TaskGroupWaitIsScopedToItsOwnTasks) {
  ThreadPool *pool = ThreadPool::get();
  if (pool->workerCount() < 2)
    GTEST_SKIP() << "Scoped wait requires at least two worker threads.";

  std::promise<void> release;
  auto gate = release.get_future().share();
  std::atomic<bool> blocker_started(false);
  std::atomic<bool> blocker_finished(false);
  std::atomic<bool> fast_finished(false);

  auto blocker_group = pool->makeTaskGroup();
  blocker_group.async([&]() {
    blocker_started.store(true, std::memory_order_release);
    gate.wait();
    blocker_finished.store(true, std::memory_order_release);
  });

  for (int spin = 0; spin < 100 && !blocker_started.load(std::memory_order_acquire);
       ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(blocker_started.load(std::memory_order_acquire));

  auto fast_group = pool->makeTaskGroup();
  fast_group.async([&]() { fast_finished.store(true, std::memory_order_release); });
  fast_group.wait();

  EXPECT_TRUE(fast_finished.load(std::memory_order_acquire));
  EXPECT_FALSE(blocker_finished.load(std::memory_order_acquire));

  release.set_value();
  blocker_group.wait();
  EXPECT_TRUE(blocker_finished.load(std::memory_order_acquire));
}

TEST(ThreadPoolHarnessTest, TaskGroupRethrowsFirstWorkerException) {
  ThreadPool *pool = ThreadPool::get();
  auto group = pool->makeTaskGroup();
  group.async([]() { throw std::runtime_error("boom"); });
  EXPECT_THROW(group.wait(), std::runtime_error);
}

TEST(ThreadPoolHarnessTest, TaskGroupSetupFailureDoesNotLeavePhantomPendingWork) {
  ThreadPool *pool = ThreadPool::get();

  {
    auto group = pool->makeTaskGroup();
    ThrowingBinderTask task;
    EXPECT_THROW(group.async(task), std::runtime_error);
  }

  {
    auto group = pool->makeTaskGroup();
    lotus::CancellationSource cancel;
    ThrowingBinderTask task;
    EXPECT_THROW(group.async(cancel.token(), task), std::runtime_error);
  }
}

TEST(ThreadPoolHarnessTest, ParallelForCoversEachIndexExactlyOnce) {
  ThreadPool *pool = ThreadPool::get();
  std::vector<std::atomic<int>> counts(37);
  for (auto &count : counts)
    count.store(0, std::memory_order_relaxed);

  pool->parallelFor<std::size_t>(0, counts.size(), 5, [&](std::size_t index) {
    counts[index].fetch_add(1, std::memory_order_relaxed);
  });

  for (const auto &count : counts)
    EXPECT_EQ(count.load(std::memory_order_relaxed), 1);
}

TEST(ThreadPoolHarnessTest, ParallelForEachPreservesCoverageOnUnevenRanges) {
  ThreadPool *pool = ThreadPool::get();
  std::vector<int> values;
  for (int i = 0; i < 23; ++i)
    values.push_back(i);

  std::vector<std::atomic<int>> seen(values.size());
  for (auto &entry : seen)
    entry.store(0, std::memory_order_relaxed);

  pool->parallelForEach(values, 4, [&](int value) {
    seen[static_cast<std::size_t>(value)].fetch_add(1, std::memory_order_relaxed);
  });

  for (const auto &entry : seen)
    EXPECT_EQ(entry.load(std::memory_order_relaxed), 1);
}

TEST(ThreadPoolHarnessTest, ParallelForEachSupportsProxyReferenceRanges) {
  ThreadPool *pool = ThreadPool::get();
  std::vector<bool> values(19, false);

  pool->parallelForEach(values, 4, [](auto bit) { bit = true; });

  for (bool value : values)
    EXPECT_TRUE(value);
}

TEST(ThreadPoolHarnessTest,
     TypedThreadLocalCreatesPerThreadInstancesAndDestroysThemWhenOwnedSlotDies) {
  ThreadPool *pool = ThreadPool::get();
  std::atomic<int> next_token(0);
  auto destroyed = std::make_shared<std::atomic<int>>(0);
  std::size_t participating_workers = 0;

  {
    auto slot = pool->makeThreadLocal<SlotValue>([&]() {
      return SlotValue(next_token.fetch_add(1, std::memory_order_relaxed) + 1,
                       destroyed);
    });

    const int main_token = slot.get().token;
    EXPECT_GT(main_token, 0);

    std::map<std::thread::id, int> worker_tokens;
    if (pool->hasWorkers()) {
      std::mutex worker_mutex;
      auto group = pool->makeTaskGroup();
      for (int i = 0; i < 32; ++i) {
        group.async([&]() {
          const auto &value = slot.get();
          std::lock_guard<std::mutex> lock(worker_mutex);
          worker_tokens[std::this_thread::get_id()] = value.token;
        });
      }
      group.wait();

      std::set<int> unique_tokens;
      for (const auto &entry : worker_tokens) {
        unique_tokens.insert(entry.second);
        EXPECT_NE(entry.second, main_token);
      }
      participating_workers = worker_tokens.size();
      EXPECT_EQ(unique_tokens.size(), worker_tokens.size());
      EXPECT_EQ(next_token.load(std::memory_order_relaxed),
                static_cast<int>(1 + participating_workers));
    } else {
      EXPECT_EQ(next_token.load(std::memory_order_relaxed), 1);
    }
  }

  EXPECT_EQ(destroyed->load(std::memory_order_relaxed),
            static_cast<int>(1 + participating_workers));
}

TEST(ThreadPoolHarnessTest, NestedForkJoinMakesProgressInsideWorkerWait) {
  ThreadPool *pool = ThreadPool::get();
  if (!pool->hasWorkers())
    GTEST_SKIP() << "Nested fork-join coverage requires worker threads.";

  std::atomic<int> child_runs(0);
  auto outer = pool->makeTaskGroup();
  outer.async([&]() {
    auto inner = pool->makeTaskGroup();
    for (int i = 0; i < 8; ++i) {
      inner.async(
          [&]() { child_runs.fetch_add(1, std::memory_order_relaxed); });
    }
    inner.wait();
  });

  outer.wait();
  EXPECT_EQ(child_runs.load(std::memory_order_relaxed), 8);
}

TEST(ThreadPoolHarnessTest, PreCancelledParallelForSkipsBody) {
  ThreadPool *pool = ThreadPool::get();
  lotus::CancellationSource cancel;
  cancel.cancel();

  std::atomic<int> runs(0);
  pool->parallelFor<int>(0, 32, 4, cancel.token(), [&](int) {
    runs.fetch_add(1, std::memory_order_relaxed);
  });

  EXPECT_EQ(runs.load(std::memory_order_relaxed), 0);
}

TEST(ThreadPoolHarnessTest, PreCancelledTaskGroupReturnsDefaultFutureValue) {
  ThreadPool *pool = ThreadPool::get();
  lotus::CancellationSource cancel;
  cancel.cancel();

  auto group = pool->makeTaskGroup();
  auto future = group.async(cancel.token(), []() { return 42; });
  EXPECT_THROW(future.get(), lotus::TaskCancelledError);
  EXPECT_THROW(group.wait(), lotus::TaskCancelledError);
}

TEST(ThreadPoolHarnessTest,
     CancelledTokenRemainsCancelledAfterSourceDestruction) {
  ThreadPool *pool = ThreadPool::get();
  lotus::CancellationToken token;
  {
    lotus::CancellationSource cancel;
    token = cancel.token();
    cancel.cancel();
  }

  EXPECT_TRUE(static_cast<bool>(token));
  EXPECT_TRUE(token.isCancelled());

  std::atomic<int> runs(0);
  pool->parallelFor<int>(0, 16, 4, token, [&](int) {
    runs.fetch_add(1, std::memory_order_relaxed);
  });
  EXPECT_EQ(runs.load(std::memory_order_relaxed), 0);

  auto group = pool->makeTaskGroup();
  auto future = group.async(token, []() { return 7; });
  EXPECT_THROW(future.get(), lotus::TaskCancelledError);
  EXPECT_THROW(group.wait(), lotus::TaskCancelledError);
}

TEST(ThreadPoolHarnessTest, ParallelReduceMatchesSequentialSum) {
  ThreadPool *pool = ThreadPool::get();
  const int reduced = pool->parallelReduce<int>(
      0, 100, 7, 0, [](int index) { return index + 1; },
      [](int acc, int value) { return acc + value; });
  EXPECT_EQ(reduced, 5050);
}

TEST(ThreadPoolHarnessTest, ParallelReduceAppliesInitialAccumulatorOnce) {
  ThreadPool *pool = ThreadPool::get();
  const int reduced = pool->parallelReduce<int>(
      0, 10, 3, 100, [](int index) { return index; },
      [](int acc, int value) { return acc + value; });
  EXPECT_EQ(reduced, 145);
}

TEST(ThreadPoolHarnessTest,
     ParallelReducePreservesSerialOrderForNonCommutativeReducers) {
  ThreadPool *pool = ThreadPool::get();
  const std::string reduced = pool->parallelReduce<int>(
      0, 6, 2, std::string(),
      [](int index) { return std::to_string(index); },
      [](std::string acc, const std::string &value) {
        acc += value;
        return acc;
      });
  EXPECT_EQ(reduced, "012345");
}

TEST(ThreadPoolHarnessTest, ThreadLocalReducerMergesPerThreadSets) {
  ThreadPool *pool = ThreadPool::get();
  auto reducer = pool->makeThreadLocalReducer<std::set<int>>(
      [](std::set<int> acc, const std::set<int> &value) {
        acc.insert(value.begin(), value.end());
        return acc;
      });

  pool->parallelFor<int>(0, 16, 2, [&](int index) {
    reducer.local().insert(index);
  });

  EXPECT_EQ(reducer.reduce(std::set<int>()),
            (std::set<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                           15}));
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv, "ThreadPool harness\n");
  return RUN_ALL_TESTS();
}
