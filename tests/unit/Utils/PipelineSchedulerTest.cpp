#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include "TestUtils/LLVMHelpers.h"

#include <chrono>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <gtest/gtest.h>

namespace {

class LeafFailure : public std::runtime_error {
public:
  LeafFailure() : std::runtime_error("leaf failure") {}
};

TEST(PipelineSchedulerTest, BottomUpSchedulingRespectsAcyclicDependencies) {
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
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 3u);
  EXPECT_EQ(Visited[0], "leaf");
  EXPECT_EQ(Visited[1], "mid");
  EXPECT_EQ(Visited[2], "root");
}

TEST(PipelineSchedulerTest, BottomUpSchedulingHandlesRecursiveCycles) {
  static constexpr const char *IR = R"IR(
    define void @entry() {
    entry:
      call void @a()
      ret void
    }

    define void @a() {
    entry:
      call void @b()
      ret void
    }

    define void @b() {
    entry:
      call void @a()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 3u);
  EXPECT_EQ(Visited[0], "a");
  EXPECT_EQ(Visited[1], "b");
  EXPECT_EQ(Visited[2], "entry");
}

TEST(PipelineSchedulerTest, TopDownSchedulingRespectsAcyclicDependencies) {
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
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_TopDown);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 3u);
  EXPECT_EQ(Visited[0], "root");
  EXPECT_EQ(Visited[1], "mid");
  EXPECT_EQ(Visited[2], "leaf");
}

TEST(PipelineSchedulerTest, BottomUpSchedulingHandlesMixedLeafAndRecursiveScc) {
  static constexpr const char *IR = R"IR(
    define void @leaf() {
    entry:
      ret void
    }

    define void @a() {
    entry:
      call void @b()
      ret void
    }

    define void @b() {
    entry:
      call void @a()
      ret void
    }

    define void @root() {
    entry:
      call void @leaf()
      call void @a()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();

  ASSERT_EQ(Visited.size(), 4u);
  const auto leaf_pos =
      std::find(Visited.begin(), Visited.end(), "leaf") - Visited.begin();
  const auto a_pos =
      std::find(Visited.begin(), Visited.end(), "a") - Visited.begin();
  const auto b_pos =
      std::find(Visited.begin(), Visited.end(), "b") - Visited.begin();
  const auto root_pos =
      std::find(Visited.begin(), Visited.end(), "root") - Visited.begin();

  ASSERT_LT(leaf_pos, Visited.size());
  ASSERT_LT(a_pos, Visited.size());
  ASSERT_LT(b_pos, Visited.size());
  ASSERT_LT(root_pos, Visited.size());
  EXPECT_LT(leaf_pos, root_pos);
  EXPECT_LT(a_pos, root_pos);
  EXPECT_LT(b_pos, root_pos);
  EXPECT_LT(a_pos, b_pos);
}

TEST(PipelineSchedulerTest, FlushesTrailingGarbageCollectionBatch) {
  static constexpr const char *IR = R"IR(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);
  Scheduler.setGCBatchSize(1000);

  std::set<std::string> Released;
  Scheduler.setTaskCallback([](const llvm::Function *) {});
  Scheduler.setGCCallback([&](const llvm::Function *F) {
    Released.insert(F->getName().str());
  });

  Scheduler.run();

  EXPECT_EQ(Released, (std::set<std::string>{"callee", "caller"}));
}

TEST(PipelineSchedulerTest, LocalSchedulingRunsGarbageCollectionCallbacks) {
  static constexpr const char *IR = R"IR(
    define void @callee() {
    entry:
      ret void
    }

    define void @caller() {
    entry:
      call void @callee()
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setGCBatchSize(1000);

  std::set<std::string> Released;
  Scheduler.setTaskCallback([](const llvm::Function *) {});
  Scheduler.setGCCallback([&](const llvm::Function *F) {
    Released.insert(F->getName().str());
  });

  Scheduler.run();

  EXPECT_EQ(Released, (std::set<std::string>{"callee", "caller"}));
}

TEST(PipelineSchedulerTest, TopDownSchedulerCanBeReusedAcrossRuns) {
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
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_TopDown);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  Scheduler.run();
  EXPECT_EQ(Visited, (std::vector<std::string>{"root", "mid", "leaf"}));

  Visited.clear();
  Scheduler.run();
  EXPECT_EQ(Visited, (std::vector<std::string>{"root", "mid", "leaf"}));
}

TEST(PipelineSchedulerTest, BottomUpFailureDoesNotWaitForUnschedulableSCCs) {
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
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_BottomUp);
  Scheduler.setTaskTimeout(1);
  Scheduler.setTaskCallback([](const llvm::Function *F) {
    if (F->getName() == "leaf")
      throw LeafFailure();
  });

  const auto Start = std::chrono::steady_clock::now();
  EXPECT_THROW(Scheduler.run(), LeafFailure);
  const auto Elapsed = std::chrono::steady_clock::now() - Start;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(Elapsed),
            std::chrono::milliseconds(1500));
}

TEST(PipelineSchedulerTest, SchedulerCanRecoverAfterFailedRun) {
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
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);
  Scheduler.setTaskCallback([](const llvm::Function *F) {
    if (F->getName() == "mid")
      throw LeafFailure();
  });

  EXPECT_THROW(Scheduler.run(), LeafFailure);

  std::vector<std::string> Visited;
  Scheduler.setTaskCallback([&](const llvm::Function *F) {
    Visited.push_back(F->getName().str());
  });

  EXPECT_NO_THROW(Scheduler.run());
  EXPECT_EQ(Visited, (std::vector<std::string>{"leaf", "mid", "root"}));
}

TEST(PipelineSchedulerTest, DumpStatusEmitsCurrentSnapshot) {
  static constexpr const char *IR = R"IR(
    define void @root() {
    entry:
      ret void
    }
  )IR";

  llvm::LLVMContext Ctx;
  auto M = lotus::unittest::parseAssembly(Ctx, IR, "PipelineSchedulerTest");
  ASSERT_TRUE(M);

  llvm::CallGraph CG(*M);
  PipelineScheduler Scheduler(*M, CG, PipelineScheduler::AT_Local);

  testing::internal::CaptureStderr();
  Scheduler.dumpStatus();
  const std::string Output = testing::internal::GetCapturedStderr();

  EXPECT_NE(Output.find("[PipelineScheduler Status]"), std::string::npos);
  EXPECT_NE(Output.find("Finished tasks in queue: 0"), std::string::npos);
  EXPECT_NE(Output.find("Functions to release: 0"), std::string::npos);
  EXPECT_NE(Output.find("SCCs tracked: 0"), std::string::npos);
}

} // namespace
