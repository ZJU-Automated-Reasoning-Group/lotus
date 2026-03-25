#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include "TestUtils/LLVMHelpers.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <gtest/gtest.h>

namespace {

TEST(PipelineSchedulerHarnessTest,
     ActiveWorkerTaskDoesNotTripSchedulerTimeout) {
  ThreadPool *pool = ThreadPool::get();
  if (!pool->hasWorkers())
    GTEST_SKIP() << "This regression requires worker threads.";

  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @root() {
    entry:
      call void @mid()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M =
      lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerHarnessTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setTaskTimeout(0);

  std::atomic<int> executed(0);
  Scheduler.setTaskCallback([&](const llvm::Function *) {
    executed.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  });

  EXPECT_NO_THROW(Scheduler.run());
  EXPECT_EQ(executed.load(std::memory_order_relaxed), 3);
}

TEST(PipelineSchedulerHarnessTest, BottomUpStillSchedulesAllFunctions) {
  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @root() {
    entry:
      call void @mid()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M =
      lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerHarnessTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(visited.size(), 3u);
  EXPECT_EQ(visited[0], "leaf");
  EXPECT_EQ(visited[1], "mid");
  EXPECT_EQ(visited[2], "root");
}

TEST(PipelineSchedulerHarnessTest,
     TopDownGarbageCollectionWaitsForCalleeExecution) {
  ThreadPool *pool = ThreadPool::get();
  if (!pool->hasWorkers())
    GTEST_SKIP() << "This regression requires worker threads.";

  static constexpr const char *IR = R"IR(
    define void @root() {
    entry:
      call void @mid()
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @leaf() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M =
      lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerHarnessTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_TopDown);
  Scheduler.setGCBatchSize(1);

  std::mutex StateMutex;
  std::condition_variable StateCond;
  std::set<std::string> BlockedFunctions;
  std::set<std::string> ReleasedFunctions;
  std::atomic<bool> AllowCalleesToRun(false);
  std::atomic<bool> ReleasedBeforeRun(false);

  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    const std::string Name = F->getName().str();
    if (Name == "root")
      return;

    {
      std::lock_guard<std::mutex> Lock(StateMutex);
      BlockedFunctions.insert(Name);
    }
    StateCond.notify_all();

    while (!AllowCalleesToRun.load(std::memory_order_acquire))
      std::this_thread::yield();

    std::lock_guard<std::mutex> Lock(StateMutex);
    if (ReleasedFunctions.count(Name) != 0)
      ReleasedBeforeRun.store(true, std::memory_order_release);
  });

  Scheduler.setGCCallback([&](const llvm::Function *F) {
    {
      std::lock_guard<std::mutex> Lock(StateMutex);
      ReleasedFunctions.insert(F->getName().str());
    }
    StateCond.notify_all();
  });

  auto RunFuture = std::async(std::launch::async, [&]() { Scheduler.run(); });

  {
    std::unique_lock<std::mutex> Lock(StateMutex);
    ASSERT_TRUE(StateCond.wait_for(
        Lock, std::chrono::seconds(1),
        [&]() { return BlockedFunctions.count("mid") != 0; }));
  }

  {
    std::unique_lock<std::mutex> Lock(StateMutex);
    EXPECT_FALSE(StateCond.wait_for(
        Lock, std::chrono::milliseconds(250),
        [&]() { return ReleasedFunctions.count("mid") != 0; }));
    EXPECT_FALSE(StateCond.wait_for(
        Lock, std::chrono::milliseconds(250),
        [&]() { return ReleasedFunctions.count("root") != 0; }));
  }

  AllowCalleesToRun.store(true, std::memory_order_release);
  RunFuture.get();

  EXPECT_FALSE(ReleasedBeforeRun.load(std::memory_order_acquire));
  EXPECT_TRUE(ReleasedFunctions.count("mid") != 0);
  EXPECT_TRUE(ReleasedFunctions.count("leaf") != 0);
}

TEST(PipelineSchedulerHarnessTest, QueuedTasksDoNotTimeoutBeforeWorkersPickThemUp) {
  ThreadPool *pool = ThreadPool::get();
  if (pool->workerCount() < 2)
    GTEST_SKIP() << "This regression requires at least two worker threads.";

  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @root() {
    entry:
      call void @mid()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M =
      lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerHarnessTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setTaskTimeout(1);

  std::promise<void> ReleaseWorkers;
  auto ReleaseFuture = ReleaseWorkers.get_future().share();
  std::atomic<unsigned> BlockedWorkers(0);

  auto blocker_group = pool->makeTaskGroup();
  for (unsigned i = 0; i < pool->workerCount(); ++i) {
    blocker_group.async([&]() {
      BlockedWorkers.fetch_add(1, std::memory_order_release);
      ReleaseFuture.wait();
    });
  }

  for (int spin = 0;
       spin < 200 &&
       BlockedWorkers.load(std::memory_order_acquire) != pool->workerCount();
       ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(BlockedWorkers.load(std::memory_order_acquire), pool->workerCount());

  std::atomic<int> Executed(0);
  Scheduler.setTaskCallback([&](const llvm::Function *) {
    Executed.fetch_add(1, std::memory_order_relaxed);
  });

  auto RunFuture = std::async(std::launch::async, [&]() { Scheduler.run(); });

  EXPECT_EQ(RunFuture.wait_for(std::chrono::milliseconds(50)),
            std::future_status::timeout);

  ReleaseWorkers.set_value();
  blocker_group.wait();
  EXPECT_NO_THROW(RunFuture.get());
  EXPECT_EQ(Executed.load(std::memory_order_relaxed), 3);
}

TEST(PipelineSchedulerHarnessTest, QueuedTasksTimeoutWhenWorkersNeverFreeUp) {
  ThreadPool *pool = ThreadPool::get();
  if (pool->workerCount() < 2)
    GTEST_SKIP() << "This regression requires at least two worker threads.";

  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @mid() {
    entry:
      call void @leaf()
      ret void
    }

    define void @root() {
    entry:
      call void @mid()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M =
      lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerHarnessTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setTaskTimeout(0);
  Scheduler.setTaskCallback([](const llvm::Function *) {});

  std::promise<void> ReleaseWorkers;
  auto ReleaseFuture = ReleaseWorkers.get_future().share();
  std::atomic<unsigned> BlockedWorkers(0);

  auto blocker_group = pool->makeTaskGroup();
  for (unsigned i = 0; i < pool->workerCount(); ++i) {
    blocker_group.async([&]() {
      BlockedWorkers.fetch_add(1, std::memory_order_release);
      ReleaseFuture.wait();
    });
  }

  for (int spin = 0;
       spin < 200 &&
       BlockedWorkers.load(std::memory_order_acquire) != pool->workerCount();
       ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(BlockedWorkers.load(std::memory_order_acquire), pool->workerCount());

  auto RunFuture = std::async(std::launch::async, [&]() { Scheduler.run(); });
  EXPECT_EQ(RunFuture.wait_for(std::chrono::milliseconds(500)),
            std::future_status::ready);

  try {
    RunFuture.get();
    FAIL() << "scheduler should time out when queued tasks never start";
  } catch (const std::runtime_error &Err) {
    EXPECT_NE(std::string(Err.what()).find("timed out"), std::string::npos);
  }

  ReleaseWorkers.set_value();
  blocker_group.wait();
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "PipelineScheduler harness\n");
  return RUN_ALL_TESTS();
}
