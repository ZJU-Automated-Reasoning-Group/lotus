#include "Analysis/Concurrency/ConcurrencyFacade.h"

#include "TestUtils/LLVMHelpers.h"

using namespace llvm;

class ConcurrencyFacadeTest : public lotus::unittest::LlvmModuleTest {};

TEST_F(ConcurrencyFacadeTest, SummarizesOpenMPTaskGraph) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_taskloop(i8*, i32, i8*, i32, i64*, i64, i32, i32, i64)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32, i8*, i32, i8*)
    declare void @__kmpc_taskgroup(i8*, i32)
    declare i32 @__kmpc_flush(i8*)
    declare i32 @__kmpc_atomic_start()
    declare i32 @__kmpc_atomic_end()
    declare i32 @__tgt_target(i64, i8*, i32, i8**, i8**, i64*)
    declare void @__tgt_target_data_begin(i64, i8*)
    declare void @__tgt_target_data_update(i64, i8*)
    declare void @__tgt_target_data_end(i64, i8*)

    define i32 @main() {
    entry:
      call void @__kmpc_taskgroup(i8* null, i32 0)
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 0, i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_taskloop(i8* null, i32 0, i8* null, i32 0,
                                i64* null, i64 0, i32 0, i32 0, i64 0)
      call i32 @__kmpc_atomic_start()
      call i32 @__kmpc_atomic_end()
      call i32 @__kmpc_flush(i8* null)
      call i32 @__tgt_target(i64 0, i8* null, i32 0, i8** null, i8** null, i64* null)
      call void @__tgt_target_data_begin(i64 0, i8* null)
      call void @__tgt_target_data_update(i64 0, i8* null)
      call void @__tgt_target_data_end(i64 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(summary.task_count, 3u);
  EXPECT_EQ(summary.task_with_dependencies_count, 2u);
  EXPECT_EQ(summary.taskloop_count, 1u);
  EXPECT_EQ(summary.final_task_count, 0u);
  EXPECT_EQ(summary.untied_task_count, 0u);
  EXPECT_EQ(summary.detached_task_count, 0u);
  EXPECT_EQ(summary.deferred_wait_dep_count, 1u);
  EXPECT_EQ(summary.wait_boundary_count, 3u);
  EXPECT_EQ(summary.partial_wait_boundary_count, 1u);
  EXPECT_EQ(summary.taskgroup_region_count, 1u);
  EXPECT_EQ(summary.single_region_count, 0u);
  EXPECT_EQ(summary.master_region_count, 0u);
  EXPECT_EQ(summary.ordered_region_count, 0u);
  EXPECT_EQ(summary.sections_region_count, 0u);
  EXPECT_EQ(summary.worksharing_loop_count, 0u);
  EXPECT_EQ(summary.reduction_region_count, 0u);
  EXPECT_EQ(summary.atomic_region_count, 1u);
  EXPECT_EQ(summary.flush_count, 1u);
  EXPECT_EQ(summary.target_region_count, 1u);
  EXPECT_EQ(summary.target_data_region_count, 3u);
  EXPECT_EQ(summary.detach_completion_count, 0u);
  EXPECT_EQ(summary.unknown_relation_count, 2u);
  EXPECT_GE(summary.unknown_reason_bucket_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, SummarizesOutlinedOpenMPAndExtendedSyncCounters) {
  const char *source = R"(
    declare void @__kmpc_fork_call(i8*, i32, void (i32*, i32*, ...)*)
    declare i32 @__kmpc_omp_task(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskyield(i8*, i32, i32)
    declare void @__kmpc_barrier(i8*, i32)
    declare void @__kmpc_critical(i8*, i32, i8*)
    declare void @__kmpc_end_critical(i8*, i32, i8*)
    declare void @omp_set_lock(i8*)
    declare void @omp_unset_lock(i8*)
    declare i32 @__kmpc_cancellationpoint(i8*, i32, i32)
    @lock = global i8 0, align 1

    define internal void @.omp_outlined.(i32* %gtid, i32* %btid, ...) {
    entry:
      call i32 @__kmpc_omp_task(i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_taskyield(i8* null, i32 0, i32 0)
      call void @__kmpc_barrier(i8* null, i32 0)
      call void @__kmpc_critical(i8* null, i32 0, i8* @lock)
      call void @__kmpc_end_critical(i8* null, i32 0, i8* @lock)
      call void @omp_set_lock(i8* @lock)
      call void @omp_unset_lock(i8* @lock)
      call i32 @__kmpc_cancellationpoint(i8* null, i32 0, i32 0)
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

  auto summary = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(summary.task_count, 2u);
  EXPECT_EQ(summary.parallel_region_count, 2u);
  EXPECT_EQ(summary.taskyield_count, 2u);
  EXPECT_EQ(summary.barrier_count, 2u);
  EXPECT_EQ(summary.critical_region_count, 2u);
  EXPECT_EQ(summary.lock_api_count, 4u);
  EXPECT_EQ(summary.cancellation_point_count, 2u);
}

TEST_F(ConcurrencyFacadeTest, CountsSelectiveOpenMPWaitDepsAsHB) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }
    @shared = global i32 0, align 4
    @deps = global [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32,
                                      %kmp_depend_info*, i32,
                                      %kmp_depend_info*)

    define i32 @main() {
    entry:
      %dep = getelementptr inbounds [1 x %kmp_depend_info],
              [1 x %kmp_depend_info]* @deps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 1,
                                     %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %dep, i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(summary.happens_before_relation_count, 1u);
  EXPECT_EQ(summary.unknown_relation_count, 0u);
  EXPECT_EQ(summary.deferred_wait_dep_count, 0u);
}

TEST_F(ConcurrencyFacadeTest, SummarizesMPIIssues) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    @win = global i8 0, align 1

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      %flag = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req2)
      call i32 @MPI_Testany(i32 2, i8** %slot0, i32* %index, i32* %flag, i8* null)
      call i32 @MPI_Request_free(i8* %req2)
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeMPI(*module);
  EXPECT_EQ(summary.operation_count, 9u);
  EXPECT_EQ(summary.init_count, 0u);
  EXPECT_EQ(summary.finalize_count, 0u);
  EXPECT_EQ(summary.blocking_point_to_point_count, 0u);
  EXPECT_EQ(summary.nonblocking_operation_count, 3u);
  EXPECT_EQ(summary.nonblocking_point_to_point_count, 2u);
  EXPECT_EQ(summary.probe_operation_count, 0u);
  EXPECT_EQ(summary.wait_operation_count, 0u);
  EXPECT_EQ(summary.test_operation_count, 1u);
  EXPECT_EQ(summary.collective_operation_count, 1u);
  EXPECT_EQ(summary.blocking_collective_count, 0u);
  EXPECT_EQ(summary.nonblocking_collective_count, 1u);
  EXPECT_EQ(summary.request_management_count, 1u);
  EXPECT_EQ(summary.rma_window_count, 1u);
  EXPECT_EQ(summary.rma_operation_count, 1u);
  EXPECT_EQ(summary.rma_sync_count, 2u);
  EXPECT_EQ(summary.may_complete_request_count, 1u);
  EXPECT_EQ(summary.terminal_request_count, 1u);
  EXPECT_EQ(summary.orphaned_request_count, 0u);
  EXPECT_EQ(summary.collective_partial_reachability_count, 0u);
  EXPECT_EQ(summary.unsynchronized_rma_count, 0u);
  EXPECT_EQ(summary.tracked_window_count, 1u);
  EXPECT_EQ(summary.leaked_window_count, 1u);
  EXPECT_EQ(summary.collective_slot_count, 1u);
  EXPECT_EQ(summary.deferred_semantic_lowering_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, SummarizesValidatedMPIStateCounters) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      %flag = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      br i1 %is_root, label %then, label %cont

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %cont

    cont:
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Testany(i32 2, i8** %slot0, i32* %index, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeMPI(*module);
  EXPECT_GE(summary.collective_slot_count, 1u);
  EXPECT_GE(summary.deferred_semantic_lowering_count, 1u);
  EXPECT_GE(summary.rank_restricted_operation_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, SummarizesExtendedMPIProtocolCounters) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Sendrecv(i8*, i32, i32, i32, i32,
                              i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank_slot = alloca i32, align 4
      %req = alloca i8, align 1
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank_slot)
      %rank = load i32, i32* %rank_slot, align 4
      %is_root = icmp eq i32 %rank, 0
      br i1 %is_root, label %root, label %cont

    root:
      call i32 @MPI_Barrier(i8* %comm)
      br label %cont

    cont:
      call i32 @MPI_Sendrecv(i8* null, i32 1, i32 0, i32 1, i32 7,
                             i8* null, i32 1, i32 0, i32 2, i32 7,
                             i8* %comm, i8* null)
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 3, i32 9,
                              i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 -2,
                         i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeMPI(*module);
  EXPECT_EQ(summary.sendrecv_operation_count, 1u);
  EXPECT_EQ(summary.persistent_request_init_count, 1u);
  EXPECT_EQ(summary.request_start_count, 1u);
  EXPECT_EQ(summary.rank_restricted_operation_count, 1u);
  EXPECT_EQ(summary.wildcard_endpoint_operation_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, PrintsOpenMPSummaryReport) {
  const char *source = R"(
    declare i32 @__kmpc_omp_task_begin_if0(i8*, i32, i8*)
    declare i32 @__kmpc_omp_taskwait(i8*, i32)
    declare i32 @__tgt_target_data_update(i64, i8*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task_begin_if0(i8* null, i32 0, i8* null)
      call i32 @__kmpc_omp_taskwait(i8* null, i32 0)
      call i32 @__tgt_target_data_update(i64 0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  std::string output;
  raw_string_ostream os(output);
  concurrency::ConcurrencyFacade::printOpenMPResults(*module, os);
  os.flush();

  EXPECT_NE(output.find("OpenMP Analysis Results"), std::string::npos);
  EXPECT_NE(output.find("Tasks: 1"), std::string::npos);
  EXPECT_NE(output.find("Taskloop/taskyield: 0/0"), std::string::npos);
  EXPECT_NE(
      output.find("Scheduling boundaries (wait/partial/taskgroup): 1/0/0"),
      std::string::npos);
  EXPECT_NE(output.find("Target regions (target/target-data): 0/1"),
            std::string::npos);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
