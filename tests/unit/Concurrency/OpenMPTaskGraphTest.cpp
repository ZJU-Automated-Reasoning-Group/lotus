#include "Analysis/Concurrency/OpenMP/OpenMPTaskGraph.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;
using namespace OpenMP;

class OpenMPTaskGraphTest : public lotus::unittest::LlvmModuleTest {};

TEST_F(OpenMPTaskGraphTest, ParsesStackBuiltDependencies) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %deps1 = alloca [1 x %kmp_depend_info], align 8
      %deps2 = alloca [1 x %kmp_depend_info], align 8

      %d10 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 0
      %d11 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 1
      %d12 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 2
      store i8* bitcast (i32* @shared to i8*), i8** %d10, align 8
      store i64 4, i64* %d11, align 8
      store i8 2, i8* %d12, align 1

      %d20 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 0
      %d21 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 1
      %d22 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 2
      store i8* bitcast (i32* @shared to i8*), i8** %d20, align 8
      store i64 4, i64* %d21, align 8
      store i8 1, i8* %d22, align 1

      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* %deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* %deps2, i64 0, i64 0

      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_EQ(tasks[0]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[1]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[0]->dependencies[0].proof, DependencyProof::Definite);
  EXPECT_EQ(tasks[0]->dependencies[0].source_kind,
            DependencySourceKind::DirectAddress);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, DisjointOffsetsDoNotConflict) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @buffer = global [2 x i32] zeroinitializer, align 4
    @deps1 = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* getelementptr inbounds ([2 x i32], [2 x i32]* @buffer, i64 0, i64 0) to i8*),
        i64 4,
        i8 2
      }
    ]
    @deps2 = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* getelementptr inbounds ([2 x i32], [2 x i32]* @buffer, i64 0, i64 1) to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps2, i64 0, i64 0
      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, ConflictingHelperTasksShareSchedulingContext) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define void @producer() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      %t = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret void
    }

    define void @consumer() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      %t = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret void
    }

    define i32 @main() {
    entry:
      call void @producer()
      call void @consumer()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest,
       RepeatedForkedOutlinedTaskBodiesAreTrackedPerCallSite) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)

    define internal void @.omp_outlined.(i32* %gtid, i32* %btid, ...) {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret void
    }

    define i32 @main() {
    entry:
      call void @__kmpc_fork_call(i8* null, i32 0,
                                  void (i32*, i32*, ...)* @.omp_outlined.)
      call void @__kmpc_fork_call(i8* null, i32 0,
                                  void (i32*, i32*, ...)* @.omp_outlined.)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  EXPECT_EQ(graph.getAllTasks().size(), 2u);
  EXPECT_EQ(graph.getSummary().parallel_region_count, 2u);
}

TEST_F(OpenMPTaskGraphTest, MutexInoutsetCreatesExclusionWithoutHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps1 = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 4
      }
    ]
    @deps2 = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 4
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info],
               [1 x %kmp_depend_info]* @deps2, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  ASSERT_EQ(tasks[0]->dependencies.size(), 1u);
  ASSERT_EQ(tasks[1]->dependencies.size(), 1u);
  EXPECT_EQ(tasks[0]->dependencies[0].type, DependType::MUTEXINOUTSET);
  EXPECT_EQ(tasks[1]->dependencies[0].type, DependType::MUTEXINOUTSET);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.happensBefore(tasks[1].get(), tasks[0].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, TaskwaitOrdersLaterTasksInSameContext) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)
    declare void @__kmpc_taskgroup(i8*, i32)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, SingleEndActsAsSchedulingBoundaryForTasks) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_single(i8*, i32)
    declare void @__kmpc_end_single(i8*, i32)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
             [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      %t1 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      %single = call i32 @__kmpc_single(i8* null, i32 0)
      call void @__kmpc_end_single(i8* null, i32 0)
      %t2 = call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, WaitDepsDoesNotImposeFullTaskwaitBarrier) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 0, i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, WaitDepsSelectivelyOrdersMatchingDependencies) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, %kmp_depend_info*, i32, %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 1,
                                     %kmp_depend_info* %dep, i32 0,
                                     %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::HappensBefore);
  EXPECT_EQ(graph.getDeferredWaitDepsCount(), 0u);
}

TEST_F(OpenMPTaskGraphTest, IndirectDependencyPointerIsLowered) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %slot = alloca %kmp_depend_info*, align 8
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      store %kmp_depend_info* %dep, %kmp_depend_info** %slot, align 8
      %loaded = load %kmp_depend_info*, %kmp_depend_info** %slot, align 8
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %loaded, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 1u);
  ASSERT_EQ(tasks.front()->dependencies.size(), 1u);
  EXPECT_NE(tasks.front()->dependencies.front().canonical_base, nullptr);
}

TEST_F(OpenMPTaskGraphTest, FlushSelectivelyOrdersMatchingTaskDependencies) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_flush(i8*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_flush(i8* bitcast (i32* @shared to i8*))
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, NestedTaskgroupDoesNotSuppressSiblingDependencies) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, TaskloopCreatesTrackedTaskNode) {
  const char *source = R"(
    declare i32 @__kmpc_taskloop(i8*, i32, i8*, i32, i64*, i64, i32, i32, i64)

    define i32 @main() {
    entry:
      call i32 @__kmpc_taskloop(i8* null, i32 0, i8* null, i32 0,
                                i64* null, i64 0, i32 0, i32 0, i64 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 1u);
  EXPECT_NE(tasks.front()->task_create, nullptr);
}

TEST_F(OpenMPTaskGraphTest, InlineTaskRuntimeVariantIsTracked) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task_begin_if0(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  EXPECT_EQ(graph.getAllTasks().size(), 1u);
  EXPECT_EQ(graph.getAllTasks().front()->execution_mode,
            TaskExecutionMode::Included);
}

TEST_F(OpenMPTaskGraphTest,
       SameBaseUnknownOffsetsAreDeferredWithoutDefiniteHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @buffer = global [8 x i8] zeroinitializer, align 1

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main(i64 %idx1, i64 %idx2) {
    entry:
      %deps1 = alloca [1 x %kmp_depend_info], align 8
      %deps2 = alloca [1 x %kmp_depend_info], align 8

      %addr1 = getelementptr inbounds [8 x i8], [8 x i8]* @buffer, i64 0, i64 %idx1
      %addr2 = getelementptr inbounds [8 x i8], [8 x i8]* @buffer, i64 0, i64 %idx2

      %d10 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 0
      %d11 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 1
      %d12 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 2
      store i8* %addr1, i8** %d10, align 8
      store i64 1, i64* %d11, align 8
      store i8 2, i8* %d12, align 1

      %d20 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 0
      %d21 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 1
      %d22 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 2
      store i8* %addr2, i8** %d20, align 8
      store i64 1, i64* %d21, align 8
      store i8 2, i8* %d22, align 1

      %dep1 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0

      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_FALSE(graph.mayBeParallel(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_GT(graph.getDeferredImpreciseConflictCount(), 0u);
  auto it = graph.getUnknownReasonCounts().find("omp_depend_may_conflict");
  ASSERT_NE(it, graph.getUnknownReasonCounts().end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(OpenMPTaskGraphTest, DistinctSummaryBasesStayConservative) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @slot1 = global i8* bitcast (i32* @shared to i8*)
    @slot2 = global i8* bitcast (i32* @shared to i8*)

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %deps1 = alloca [1 x %kmp_depend_info], align 8
      %deps2 = alloca [1 x %kmp_depend_info], align 8

      %addr1 = load i8*, i8** @slot1, align 8
      %addr2 = load i8*, i8** @slot2, align 8

      %d10 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 0
      %d11 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 1
      %d12 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0, i32 2
      store i8* %addr1, i8** %d10, align 8
      store i64 4, i64* %d11, align 8
      store i8 2, i8* %d12, align 1

      %d20 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 0
      %d21 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 1
      %d22 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0, i32 2
      store i8* %addr2, i8** %d20, align 8
      store i64 4, i64* %d21, align 8
      store i8 2, i8* %d22, align 1

      %dep1 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps1, i64 0, i64 0
      %dep2 = getelementptr inbounds [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %deps2, i64 0, i64 0

      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep2, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  auto it =
      graph.getUnknownReasonCounts().find("omp_depend_distinct_base_may_alias");
  ASSERT_NE(it, graph.getUnknownReasonCounts().end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(OpenMPTaskGraphTest, NestedTaskgroupBoundaryAdvancesOnlyInnerPhase) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare void @__kmpc_taskgroup(i8*, i32)
    declare void @__kmpc_end_taskgroup(i8*, i32)
    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call void @__kmpc_end_taskgroup(i8* null, i32 0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 3u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_TRUE(graph.happensBefore(tasks[1].get(), tasks[2].get()));
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[2].get()));
}

TEST_F(OpenMPTaskGraphTest, ConditionalTaskwaitDoesNotCreateDefiniteHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %merge

    then:
      %dep0 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep0, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      br label %merge

    merge:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
}

TEST_F(OpenMPTaskGraphTest, ConflictingConditionalTasksStayUnknown) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info {
        i8* bitcast (i32* @shared to i8*),
        i64 4,
        i8 2
      }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right

    left:
      %dep0 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep0, i32 0, %kmp_depend_info* null)
      ret i32 0

    right:
      %dep1 = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(
          i8* null, i32 0, i8* null, i32 1,
          %kmp_depend_info* %dep1, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
}

TEST_F(OpenMPTaskGraphTest, BarrierCreatesDefiniteBoundaryAcrossTasks) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_barrier(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call void @__kmpc_barrier(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.getSummary().barrier_count, 1u);
}

TEST_F(OpenMPTaskGraphTest, DoacrossWaitWithoutWitnessStaysPartialUnknown) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare void @__kmpc_doacross_wait(i8*, i32, i64*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call void @__kmpc_doacross_wait(i8* null, i32 0, i64* null)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  auto it = graph.getDeferredReasonCounts().find("omp_doacross_partial");
  ASSERT_NE(it, graph.getDeferredReasonCounts().end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(OpenMPTaskGraphTest, TargetDataEndNowaitProducesPartialBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end_nowait(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__tgt_target_data_end_nowait(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_GT(graph.getSummary().target_nowait_boundary_count, 0u);
}

TEST_F(OpenMPTaskGraphTest, TargetDataEndCreatesDefiniteBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__tgt_target_data_end(i8*, i32)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__tgt_target_data_end(i8* null, i32 0)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_TRUE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
}

TEST_F(OpenMPTaskGraphTest, ReduceNowaitEndProducesPartialBoundary) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_reduce_nowait(i8*, i32, i32, i64, i8*, i8*, i8*)
    declare i32 @__kmpc_end_reduce_nowait(i8*, i32, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__kmpc_reduce_nowait(i8* null, i32 0, i32 1, i64 4,
                                     i8* null, i8* null, i8* null)
      call i32 @__kmpc_end_reduce_nowait(i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPTaskGraph graph(*module);
  graph.analyze();

  const auto &tasks = graph.getAllTasks();
  ASSERT_EQ(tasks.size(), 2u);
  EXPECT_FALSE(graph.happensBefore(tasks[0].get(), tasks[1].get()));
  EXPECT_EQ(graph.classifyTaskRelation(tasks[0].get(), tasks[1].get()),
            OpenMPTaskGraph::TaskRelation::Unknown);
  EXPECT_GT(graph.getSummary().reduction_nowait_boundary_count, 0u);
}

TEST_F(OpenMPTaskGraphTest, DependencyConflictClassificationDelegatesToSemantics) {
  const char *source = R"(
    define i32 @main() {
    entry:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  OpenMPSemantics semantics(*module);
  OpenMPTaskGraph graph(*module);
  semantics.analyze();

  Dependency lhs;
  lhs.type = DependType::OUT;
  lhs.address = nullptr;
  lhs.size = 4;

  Dependency rhs = lhs;

  EXPECT_EQ(graph.classifyDependencyConflict(lhs, rhs),
            semantics.classifyDependencyConflictForTesting(lhs, rhs));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
