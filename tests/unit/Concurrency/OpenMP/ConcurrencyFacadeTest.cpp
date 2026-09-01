#include "Concurrency/ConcurrencyFacade.h"
#include "Concurrency/CUDA/CUDAAnalysis.h"
#include "Concurrency/MPI/MPIAnalysis.h"
#include "Concurrency/Utils/ThreadAPI.h"

#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/IRBuilder.h>

using namespace llvm;

class ConcurrencyFacadeTest : public lotus::unittest::LlvmModuleTest {};

class ThreadAPIConfigGuard {
public:
  ThreadAPIConfigGuard()
      : m_config(ThreadAPI::getThreadAPI()->getConfig()) {}
  ~ThreadAPIConfigGuard() { ThreadAPI::getThreadAPI()->setConfig(m_config); }

private:
  concurrency::ConcurrencyConfig m_config;
};

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
  EXPECT_EQ(summary.partial_wait_boundary_count, 2u);
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
  EXPECT_GE(summary.getRelationCount(
                concurrency::RelationKind::UnknownDueToModelGap,
                concurrency::ProofStrength::Unknown),
            1u);
  EXPECT_GE(summary.unknown_reason_counts["omp_taskwait_deps_partial"], 1u);
}

TEST_F(ConcurrencyFacadeTest, GenericAndDisabledOpenMPAreNotApplicable) {
  ThreadAPIConfigGuard config_guard;
  auto module = parseModule(R"(
    define i32 @main(i1 %cond) {
    entry:
      br i1 %cond, label %left, label %right
    left:
      ret i32 1
    right:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  auto generic = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(generic.status,
            concurrency::ConcurrencyFacade::OpenMPAnalysisStatus::NotApplicable);
  EXPECT_TRUE(generic.unknown_reason_counts.empty());

  concurrency::ConcurrencyConfig config = ThreadAPI::getThreadAPI()->getConfig();
  config.set_enable_openmp(false);
  ThreadAPI::getThreadAPI()->setConfig(config);
  auto disabled = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(disabled.status,
            concurrency::ConcurrencyFacade::OpenMPAnalysisStatus::Disabled);
  EXPECT_TRUE(disabled.unknown_reason_counts.empty());
}

TEST_F(ConcurrencyFacadeTest, RelationMergeKeepsAllReasonsAndWeakestProof) {
  concurrency::Relation relation;
  relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
  relation.proof = concurrency::ProofStrength::May;
  relation.reason = "may_alias";
  concurrency::addRelationEvidence(relation, relation.reason);

  concurrency::Relation later;
  later.kind = concurrency::RelationKind::UnknownDueToModelGap;
  later.proof = concurrency::ProofStrength::Unknown;
  later.reason = "partial_wait";
  concurrency::mergeSameKindRelation(relation, later);

  EXPECT_EQ(relation.proof, concurrency::ProofStrength::Unknown);
  EXPECT_EQ(relation.reason, "partial_wait");
  EXPECT_EQ(relation.evidence_reasons.size(), 2u);
}

TEST_F(ConcurrencyFacadeTest, PreservesOpenMPMayGapReasonAndProof) {
  const char *source = R"(
    %kmp_depend_info = type { i8*, i64, i8 }

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)

    define i32 @main(i8* %lhs_addr, i8* %rhs_addr) {
    entry:
      %lhsdeps = alloca [1 x %kmp_depend_info], align 8
      %rhsdeps = alloca [1 x %kmp_depend_info], align 8
      %l0 = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0, i32 0
      %l1 = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0, i32 1
      %l2 = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0, i32 2
      %r0 = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0, i32 0
      %r1 = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0, i32 1
      %r2 = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0, i32 2
      store i8* %lhs_addr, i8** %l0
      store i64 4, i64* %l1
      store i8 2, i8* %l2
      store i8* %rhs_addr, i8** %r0
      store i64 4, i64* %r1
      store i8 2, i8* %r2
      %lhs = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %lhsdeps, i64 0, i64 0
      %rhs = getelementptr [1 x %kmp_depend_info], [1 x %kmp_depend_info]* %rhsdeps, i64 0, i64 0
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %lhs, i32 0,
                                          %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* %rhs, i32 0,
                                          %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(summary.getRelationCount(
                concurrency::RelationKind::UnknownDueToModelGap,
                concurrency::ProofStrength::May),
            1u);
  EXPECT_EQ(summary.getRelationCount(
                concurrency::RelationKind::UnknownDueToModelGap,
                concurrency::ProofStrength::Unknown),
            0u);
  EXPECT_GE(summary.unknown_reason_counts["omp_depend_may_conflict"], 1u);
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
    @lhs_shared = global i32 0, align 4
    @rhs_shared = global i32 0, align 4
    @lhsdeps = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @lhs_shared to i8*), i64 4, i8 2 }
    ]
    @rhsdeps = constant [1 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @rhs_shared to i8*), i64 4, i8 2 }
    ]
    @waitdeps = constant [2 x %kmp_depend_info] [
      %kmp_depend_info { i8* bitcast (i32* @lhs_shared to i8*), i64 4, i8 2 },
      %kmp_depend_info { i8* bitcast (i32* @rhs_shared to i8*), i64 4, i8 2 }
    ]

    declare i32 @__kmpc_omp_task_with_deps(i8*, i32, i8*, i32,
                                           %kmp_depend_info*, i32,
                                           %kmp_depend_info*)
    declare i32 @__kmpc_omp_wait_deps(i8*, i32, i32,
                                      %kmp_depend_info*, i32,
                                      %kmp_depend_info*)

    define i32 @main() {
    entry:
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* getelementptr ([1 x %kmp_depend_info], [1 x %kmp_depend_info]* @lhsdeps, i64 0, i64 0), i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_wait_deps(i8* null, i32 0, i32 2,
                                     %kmp_depend_info* getelementptr ([2 x %kmp_depend_info], [2 x %kmp_depend_info]* @waitdeps, i64 0, i64 0), i32 0, %kmp_depend_info* null)
      call i32 @__kmpc_omp_task_with_deps(i8* null, i32 0, i8* null, i32 1,
                                          %kmp_depend_info* getelementptr ([1 x %kmp_depend_info], [1 x %kmp_depend_info]* @rhsdeps, i64 0, i64 0), i32 0, %kmp_depend_info* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeOpenMP(*module);
  EXPECT_EQ(summary.getRelationCount(
                concurrency::RelationKind::SelectiveHappenBefore,
                concurrency::ProofStrength::Must),
            1u);
  EXPECT_EQ(summary.getRelationCount(
                concurrency::RelationKind::MustHappenBefore,
                concurrency::ProofStrength::Must),
            0u);
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

TEST_F(ConcurrencyFacadeTest, PreservesMPIDiagnosticAndModelGapMetadata) {
  const char *source = R"(
    declare i32 @MPI_Init(i32*, i8***)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm, i32 %flag_value) {
    entry:
      %reqs = alloca [1 x i8*], align 8
      %slot = getelementptr inbounds [1 x i8*], [1 x i8*]* %reqs, i64 0, i64 0
      %index = alloca i32, align 4
      %flag = alloca i32, align 4
      store i8* null, i8** %slot, align 8
      store i32 %flag_value, i32* %flag, align 4
      call i32 @MPI_Init(i32* null, i8*** null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -3, i32 -2, i8* %comm)
      call i32 @MPI_Testany(i32 1, i8** %slot, i32* %index,
                            i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeMPI(*module);
  EXPECT_TRUE(summary.missing_finalize);
  EXPECT_EQ(summary.invalid_tag_count, 1u);
  EXPECT_EQ(summary.invalid_rank_count, 1u);
  EXPECT_GE(summary.model_gap_count, 1u);
  EXPECT_EQ(summary.diagnostic_code_counts["missing_finalize"], 1u);
  EXPECT_EQ(summary.diagnostic_code_counts["invalid_tag"], 1u);
  EXPECT_EQ(summary.diagnostic_code_counts["invalid_rank"], 1u);

  bool saw_model_gap_relation = false;
  bool saw_scope_metadata = false;
  for (const auto &diagnostic : summary.diagnostics) {
    if (diagnostic.has_relation &&
        diagnostic.relation.kind ==
            concurrency::RelationKind::UnknownDueToModelGap &&
        diagnostic.relation.proof == concurrency::ProofStrength::Unknown &&
        !diagnostic.model_gap_domain.empty()) {
      saw_model_gap_relation = true;
    }
    if (diagnostic.has_relation &&
        (diagnostic.communicator_class_id != 0 ||
         diagnostic.participant_class_id != 0 ||
         diagnostic.channel_class_id != 0 ||
         diagnostic.request_set_id != 0)) {
      saw_scope_metadata = true;
    }
  }
  EXPECT_TRUE(saw_model_gap_relation);
  EXPECT_TRUE(saw_scope_metadata);
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

TEST_F(ConcurrencyFacadeTest, CountsCanonicalMPIRelationsAndDeduplicatesSendrecv) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Sendrecv(i8*, i32, i32, i32, i32,
                              i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm, i8* %buf) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7,
                         i8* %comm, i8* null)
      call i32 @MPI_Sendrecv(i8* %buf, i32 1, i32 0, i32 1, i32 8,
                             i8* %buf, i32 1, i32 0, i32 2, i32 8,
                             i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);
  auto summary = concurrency::ConcurrencyFacade::analyzeMPI(*module);

  EXPECT_GE(summary.getRelationCount(
                concurrency::RelationKind::MatchedCommunication,
                concurrency::ProofStrength::Must) +
                summary.getRelationCount(
                    concurrency::RelationKind::MatchedCommunication,
                    concurrency::ProofStrength::May),
            1u);
  EXPECT_EQ(summary.sendrecv_operation_count, 1u);
  EXPECT_EQ(summary.buffer_overlap_count, 1u);
  EXPECT_EQ(summary.diagnostic_code_counts["buffer_overlap"], 1u);
}

TEST_F(ConcurrencyFacadeTest, SummarizesCUDAUnifiedMemoryAndHazards) {
  const char *source = R"(
    @global_arr = addrspace(1) global [64 x i32] zeroinitializer
    %stream_t = type opaque

    declare i64 @cudaLaunchKernel(i8*, i64, i64, i64, i64, i64,
                                  i8**, i64, %stream_t*)
    declare i32 @cudaMallocManaged(i8**, i64, i32)
    declare i32 @cudaMallocHost(i8**, i64)
    declare i32 @cudaMemPrefetchAsync(i8*, i64, i32, i8*)
    declare i32 @llvm.nvvm.read.ptx.sreg.tid.x()

    define void @kernel_producer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      store i32 %tid, i32 addrspace(1)* %gep
      ret void
    }

    define void @kernel_consumer() {
    entry:
      %tid = call i32 @llvm.nvvm.read.ptx.sreg.tid.x()
      %gep = getelementptr [64 x i32], [64 x i32] addrspace(1)* @global_arr, i32 0, i32 %tid
      %val = load i32, i32 addrspace(1)* %gep
      ret void
    }

    define i32 @main() {
    entry:
      %managed = alloca i8*
      %host = alloca i8*
      %s1 = inttoptr i64 10 to %stream_t*
      %s2 = inttoptr i64 20 to %stream_t*
      %m = call i32 @cudaMallocManaged(i8** %managed, i64 64, i32 1)
      %managed_ptr = load i8*, i8** %managed
      %p = call i32 @cudaMemPrefetchAsync(i8* %managed_ptr, i64 64, i32 2, i8* null)
      %h = call i32 @cudaMallocHost(i8** %host, i64 32)
      %l0 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel_producer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s1)
      call void @kernel_producer()
      %l1 = call i64 @cudaLaunchKernel(
          i8* bitcast (void ()* @kernel_consumer to i8*), i64 1, i64 32,
          i64 1, i64 1, i64 1, i8** null, i64 0, %stream_t* %s2)
      call void @kernel_consumer()
      %sum = add i32 %m, %p
      %sum2 = add i32 %sum, %h
      ret i32 %sum2
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeCUDA(*module);
  EXPECT_TRUE(summary.isComplete());
  EXPECT_EQ(summary.kernel_launch_count, 2u);
  EXPECT_EQ(summary.inter_kernel_hazard_count, 1u);
  EXPECT_EQ(summary.unified_memory_count, 3u);
  EXPECT_EQ(summary.managed_allocation_count, 1u);
  EXPECT_EQ(summary.unified_prefetch_count, 1u);
  EXPECT_EQ(summary.unified_host_allocation_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, CUDALegacyLaunchContributesOnce) {
  const char *source = R"(
    declare void @__set_CUDAConfig(i32, i32)

    define void @kernel() {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeCUDA(*module);
  EXPECT_TRUE(summary.isComplete());
  EXPECT_EQ(summary.kernel_launch_count, 1u);
  EXPECT_EQ(summary.operation_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, CUDASummaryUsesRunConfigurationSnapshot) {
  ThreadAPIConfigGuard config_guard;
  auto module = parseModule(R"(
    declare void @__set_CUDAConfig(i32, i32)
    define void @kernel() { ret void }
    define i32 @main() {
    entry:
      call void @__set_CUDAConfig(i32 1, i32 32)
      call void @kernel()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  concurrency::ConcurrencyConfig config = ThreadAPI::getThreadAPI()->getConfig();
  config.set_enable_cuda(false);
  ThreadAPI::getThreadAPI()->setConfig(config);

  auto summary = concurrency::ConcurrencyFacade::summarizeCUDA(analysis);
  EXPECT_EQ(summary.status,
            concurrency::ConcurrencyFacade::CUDAAnalysisStatus::Complete);
  EXPECT_EQ(summary.operation_count, 1u);
  EXPECT_EQ(summary.kernel_launch_count, 1u);
}

TEST_F(ConcurrencyFacadeTest, CUDARejectsUnrunAndMismatchedSnapshots) {
  auto module_a = parseModule("define void @a() { ret void }");
  auto module_b = parseModule("define void @b() { ret void }");
  ASSERT_NE(module_a, nullptr);
  ASSERT_NE(module_b, nullptr);

  concurrency::cuda::CUDAAnalysis analysis(*module_a);
  auto unrun = concurrency::ConcurrencyFacade::summarizeCUDA(analysis);
  EXPECT_EQ(unrun.status,
            concurrency::ConcurrencyFacade::CUDAAnalysisStatus::NotRun);
  EXPECT_EQ(unrun.operation_count, 0u);

  analysis.runAnalysis();
  auto mismatched =
      concurrency::ConcurrencyFacade::summarizeCUDA(analysis, *module_b);
  EXPECT_EQ(mismatched.status,
            concurrency::ConcurrencyFacade::CUDAAnalysisStatus::ModuleMismatch);
  EXPECT_EQ(mismatched.kernel_count, 0u);

  auto *main_function = module_a->getFunction("a");
  ASSERT_NE(main_function, nullptr);
  auto *return_inst = main_function->getEntryBlock().getTerminator();
  ASSERT_NE(return_inst, nullptr);
  llvm::IRBuilder<> builder(return_inst);
  builder.CreateAlloca(builder.getInt32Ty());
  auto stale = concurrency::ConcurrencyFacade::summarizeCUDA(analysis);
  EXPECT_EQ(stale.status,
            concurrency::ConcurrencyFacade::CUDAAnalysisStatus::StaleAnalysis);
}

TEST_F(ConcurrencyFacadeTest, CUDAHonorsDisabledSnapshotAcrossThreadAPIReset) {
  ThreadAPIConfigGuard config_guard;
  auto module = parseModule(R"(
    define ptx_kernel void @kernel() !nvvm.annotations !0 {
    entry:
      ret void
    }
    !0 = !{void ()* @kernel, !"kernel", i32 1}
  )");
  ASSERT_NE(module, nullptr);

  ThreadAPI *api = ThreadAPI::getThreadAPI();
  concurrency::ConcurrencyConfig config = api->getConfig();
  config.set_enable_cuda(false);
  api->setConfig(config);

  concurrency::cuda::CUDAAnalysis analysis(*module);
  analysis.runAnalysis();
  auto disabled = concurrency::ConcurrencyFacade::summarizeCUDA(analysis);
  EXPECT_EQ(disabled.status,
            concurrency::ConcurrencyFacade::CUDAAnalysisStatus::Disabled);
  EXPECT_EQ(disabled.kernel_count, 0u);

  ThreadAPI::resetThreadAPI();
  EXPECT_EQ(ThreadAPI::getThreadAPI(), api);
  analysis.runAnalysis();
  EXPECT_TRUE(analysis.hasCompletedAnalysis());
}

TEST_F(ConcurrencyFacadeTest, MPIAnalysisSurvivesThreadAPIReset) {
  auto module = parseModule(R"(
    declare i32 @MPI_Barrier(i8*)
    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Barrier(i8* %comm)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ThreadAPI *api = ThreadAPI::getThreadAPI();
  mpi::MPIAnalysis analysis(*module);
  ThreadAPI::resetThreadAPI();
  EXPECT_EQ(ThreadAPI::getThreadAPI(), api);
  analysis.runAnalysis();
  EXPECT_EQ(analysis.getOperationCount(mpi::MPIOpKind::BARRIER_BLOCKING), 1u);
}

TEST_F(ConcurrencyFacadeTest, PreservesCUDAModelGapConfidence) {
  const char *source = R"(
    define ptx_kernel void @kernel() !nvvm.annotations !0 {
    entry:
      ret void
    }

    !0 = !{void ()* @kernel, !"kernel", i32 1}
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  auto summary = concurrency::ConcurrencyFacade::analyzeCUDA(*module);
  EXPECT_TRUE(summary.isComplete());
  EXPECT_EQ(summary.kernel_count, 1u);
  EXPECT_GE(summary.model_gap_count, 1u);
  EXPECT_GE(summary.model_gap_reason_counts["launch_context_missing"], 1u);

  bool saw_confidence = false;
  for (const auto &gap : summary.model_gaps) {
    if (gap.reason_bucket == "launch_context_missing" &&
        gap.confidence == 0.55 && !gap.explanation.empty()) {
      saw_confidence = true;
    }
  }
  EXPECT_TRUE(saw_confidence);
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
