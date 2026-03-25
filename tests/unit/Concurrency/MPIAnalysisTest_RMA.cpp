#include "MPIAnalysisTestCommon.h"

TEST_F(MPIAnalysisTest, RMAOpBeforeLockEpochRemainsUnsynchronized) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().unsynchronized_rma.size(), 1u);
}

TEST_F(MPIAnalysisTest, RMAOpInsideLockEpochIsSynchronized) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_flush_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush_all(i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
  EXPECT_FALSE(analysis.getResults().rma_synchronization_facts.empty());
}

TEST_F(MPIAnalysisTest, RMAOpsOnOtherTargetsDoNotReusePointLockEpoch) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 0, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush(i32 0, i8* %win)
      call i32 @MPI_Win_unlock(i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().unsynchronized_rma.size(), 1u);
  const Instruction *unsync_inst = analysis.getResults().unsynchronized_rma.front().inst;
  ASSERT_NE(unsync_inst, nullptr);
  const auto *cb = dyn_cast<CallBase>(unsync_inst);
  ASSERT_NE(cb, nullptr);
  ASSERT_EQ(cb->arg_size(), 8u);
  const auto *rank = dyn_cast<ConstantInt>(cb->getArgOperand(3));
  ASSERT_NE(rank, nullptr);
  EXPECT_EQ(rank->getSExtValue(), 1);
}

TEST_F(MPIAnalysisTest, PSCWAccessEpochSynchronizesContainedOps) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %group = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_start(i8* %group, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_complete(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_FALSE(analysis.getResults().unsynchronized_rma.empty());
}

TEST_F(MPIAnalysisTest, LatestFlagStoreBeforeTestCompletesRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Test(i8*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      %flag = alloca i32, align 4
      store i32 0, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      store i32 1, i32* %flag, align 4
      call i32 @MPI_Test(i8* %req, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, DisjointRMADisplacementsDoNotRace) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 8, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().rma_races.empty());
}

TEST_F(MPIAnalysisTest, BlockingSendCycleNeedsCompatibleReceives) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().potential_deadlocks.size(), 1u);
}

TEST_F(MPIAnalysisTest, IncompatibleLaterReceivesDoNotTriggerDeadlock) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 99, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 98, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().potential_deadlocks.empty());
}

TEST_F(MPIAnalysisTest, BlockingWaitDoesNotPrematurelyDischargeChannel) {
  const char *source = R"(
    declare i32 @MPI_Irecv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Irecv(i8* null, i32 1, i32 0, i32 1, i32 1, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 2, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Irecv(i8* null, i32 1, i32 0, i32 0, i32 2, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 1, i8* %comm)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().potential_deadlocks.empty());
  EXPECT_FALSE(analysis.getRequestFacts().empty());
}

TEST_F(MPIAnalysisTest, CollectiveCountMismatchIsReportedPerCommunicatorClass) {
  const char *source = R"(
    declare i32 @MPI_Comm_dup(i8*, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_dup(i8* %comm, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %dup_loaded)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_dup(i8* %comm, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Bcast(i8* null, i32 2, i32 0, i32 0, i8* %dup_loaded)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 1u);
}

TEST_F(MPIAnalysisTest, PopulatesAdditionalMPIResultBuckets) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Request_free(i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Comm_free(i8*)

    @MPI_IN_PLACE = external global i8
    @MPI_COMM_NULL = external global i8

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 2, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      call i32 @MPI_Bcast(i8* @MPI_IN_PLACE, i32 1, i32 0, i32 -1, i8* %comm)
      call i32 @MPI_Comm_free(i8* @MPI_COMM_NULL)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().type_size_mismatches.size(), 2u);
  EXPECT_EQ(analysis.getResults().request_free_after_wait.size(), 1u);
  EXPECT_EQ(analysis.getResults().negative_root.size(), 1u);
  EXPECT_EQ(analysis.getResults().in_place_wrong_op.size(), 1u);
  EXPECT_EQ(analysis.getResults().destroy_null_comm.size(), 1u);
}

TEST_F(MPIAnalysisTest, WildcardsAreNotReportedAsInvalidRanksOrOutOfBounds) {
  const char *source = R"(
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -2, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().invalid_ranks.empty());
  EXPECT_TRUE(analysis.getResults().rank_out_of_bounds.empty());
}

TEST_F(MPIAnalysisTest, InvalidNegativeRankIsReported) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -3, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().invalid_ranks.size(), 1u);
  EXPECT_EQ(analysis.getResults().rank_out_of_bounds.size(), 1u);
}

TEST_F(MPIAnalysisTest, InvalidSendAnySourceRankIsReported) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().invalid_ranks.size(), 1u);
  EXPECT_EQ(analysis.getResults().rank_out_of_bounds.size(), 1u);
}

TEST_F(MPIAnalysisTest, RankBeyondKnownCommunicatorBoundIsReported) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 2048, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().invalid_ranks.size(), 1u);
  EXPECT_EQ(analysis.getResults().rank_out_of_bounds.size(), 1u);
}

TEST_F(MPIAnalysisTest, RootBeyondKnownCommunicatorBoundIsReported) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 2048, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().negative_root.size(), 1u);
}

TEST_F(MPIAnalysisTest, SymbolicRankRangeAllowsMayMatchClassification) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %peer = add i32 %loaded, 1
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 %peer, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 2, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  auto recvs =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::RECV_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  ASSERT_EQ(recvs.size(), 1u);
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::MayMatch);
}

TEST_F(MPIAnalysisTest, CancelWithoutWaitIsReportedAcrossControlFlow) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Cancel(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Cancel(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().cancel_without_wait.size(), 1u);
}

TEST_F(MPIAnalysisTest, PSCWExposureEpochSynchronizesContainedOps) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_post(i8*, i32, i8*)
    declare i32 @MPI_Win_wait(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %group = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_post(i8* %group, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_wait(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_FALSE(analysis.getResults().unsynchronized_rma.empty());
}

TEST_F(MPIAnalysisTest, UnresolvedWinLockDoesNotBecomeLockAll) {
  const char *source = R"(
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)

    define i32 @main(i32 %rank, i8* %win) {
    entry:
      call i32 @MPI_Win_lock(i32 0, i32 %rank, i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sync_ops =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::RMA_SYNC);
  ASSERT_EQ(sync_ops.size(), 1u);
  EXPECT_EQ(sync_ops.front().td_type, ThreadAPI::TD_MPI_WIN_LOCK);
  EXPECT_FALSE(sync_ops.front().rma_lock_all);
  EXPECT_EQ(sync_ops.front().semantic_relation.kind,
            concurrency::RelationKind::UnknownDueToModelGap);
  EXPECT_EQ(sync_ops.front().semantic_relation.reason,
            "mpi_rma_lock_target_unresolved");
}

TEST_F(MPIAnalysisTest, WinLockAllRetainsAllTargetEpochSemantics) {
  const char *source = R"(
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)

    define i32 @main(i8* %win) {
    entry:
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sync_ops =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::RMA_SYNC);
  ASSERT_EQ(sync_ops.size(), 2u);
  EXPECT_TRUE(sync_ops.front().rma_lock_all);

  bool saw_lock_all_epoch = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.code == "mpi_rma_lock_all_epoch") {
      saw_lock_all_epoch = true;
    }
  }
  EXPECT_TRUE(saw_lock_all_epoch);
}

TEST_F(MPIAnalysisTest, LonePSCWEpochProducesUnresolvedGroupFact) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %group = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_start(i8* %group, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_complete(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_unresolved_group = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.code == "mpi_rma_pscw_group_unresolved") {
      saw_unresolved_group = true;
    }
  }
  EXPECT_TRUE(saw_unresolved_group);
  EXPECT_FALSE(analysis.getResults().unsynchronized_rma.empty());
}

TEST_F(MPIAnalysisTest, BlockingAndNonBlockingCollectivesDoNotCompareAsCompatible) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibcast(i8* null, i32 1, i32 0, i32 0, i8* %comm, i8* %req)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 1u);
}

TEST_F(MPIAnalysisTest, DistinctCollectiveVariantsRemainIncompatible) {
  const char *source = R"(
    declare i32 @MPI_Alltoall(i8*, i32, i32, i8*, i32, i32, i8*)
    declare i32 @MPI_Alltoallv(i8*, i32*, i32*, i32, i8*, i32*, i32*, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Alltoall(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %counts = alloca i32, align 4
      %displs = alloca i32, align 4
      call i32 @MPI_Alltoallv(i8* null, i32* %counts, i32* %displs, i32 0,
                              i8* null, i32* %counts, i32* %displs, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 1u);
}

TEST_F(MPIAnalysisTest, DistinctPSCWGroupsStayUnresolved) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Win_post(i8*, i32, i8*)
    declare i32 @MPI_Win_wait(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @access_rank(i8* %comm) {
    entry:
      %group_a = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_start(i8* %group_a, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_complete(i8* %win)
      ret i32 0
    }

    define i32 @exposure_rank(i8* %comm) {
    entry:
      %group_b = alloca i8, align 1
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_post(i8* %group_b, i32 0, i8* %win)
      call i32 @MPI_Win_wait(i8* %win)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @access_rank(i8* %comm)
      %b = call i32 @exposure_rank(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_unresolved_group = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.code == "mpi_rma_pscw_group_unresolved") {
      saw_unresolved_group = true;
      EXPECT_NE(fact.relation.proof, concurrency::ProofStrength::Must);
    }
  }
  EXPECT_TRUE(saw_unresolved_group);
}

TEST_F(MPIAnalysisTest, PSCWGroupDowngradeLeavesConflictingPutsUnsynchronized) {
  const char *source = R"(
    @shared_win = global i8 0, align 1

    declare i32 @MPI_Win_start(i8*, i32, i8*)
    declare i32 @MPI_Win_complete(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @rank_a() {
    entry:
      %group = alloca i8, align 1
      call i32 @MPI_Win_start(i8* %group, i32 0, i8* @shared_win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0,
                        i8* @shared_win)
      call i32 @MPI_Win_complete(i8* @shared_win)
      ret i32 0
    }

    define i32 @rank_b() {
    entry:
      %group = alloca i8, align 1
      call i32 @MPI_Win_start(i8* %group, i32 0, i8* @shared_win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0,
                        i8* @shared_win)
      call i32 @MPI_Win_complete(i8* @shared_win)
      ret i32 0
    }

    define i32 @main() {
    entry:
      %a = call i32 @rank_a()
      %b = call i32 @rank_b()
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_GE(analysis.getResults().unsynchronized_rma.size(), 2u);
  EXPECT_FALSE(analysis.getResults().rma_races.empty());

  bool saw_unresolved_group = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.code == "mpi_rma_pscw_group_unresolved") {
      saw_unresolved_group = true;
    }
  }
  EXPECT_TRUE(saw_unresolved_group);
}

TEST_F(MPIAnalysisTest,
       InvalidRMAEpochTransitionLeavesOperationUnsynchronized) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Win_fence(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 0, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_fence(i32 0, i8* %win)
      call i32 @MPI_Win_unlock(i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().unsynchronized_rma.size(), 1u);
  EXPECT_EQ(analysis.getResults().invalid_rma_transitions.size(), 2u);
  const auto &relations =
      analysis.getRMAAnalysis().getSynchronizationRelations();
  ASSERT_EQ(relations.size(), 1u);
  EXPECT_EQ(relations.front().relation.kind,
            concurrency::RelationKind::UnknownDueToModelGap);
}

TEST_F(MPIAnalysisTest, UseAfterFreeWindowIsReported) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().use_after_free_windows.size(), 1u);
}

TEST_F(MPIAnalysisTest, OperationBeforeWindowFreeIsNotUseAfterFree) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().use_after_free_windows.empty());
}

TEST_F(MPIAnalysisTest, BranchSeparatedWindowFreeDoesNotReportUseAfterFree) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @main(i8* %comm, i1 %cond) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      br i1 %cond, label %free_path, label %use_path

    free_path:
      call i32 @MPI_Win_free(i8* %win)
      br label %join

    use_path:
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      br label %join

    join:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().use_after_free_windows.empty());
}

TEST_F(MPIAnalysisTest,
       CrossFunctionWindowFreeWithoutOrderingProducesModelGap) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define void @free_win() {
    entry:
      call i32 @MPI_Win_free(i8* @win)
      ret void
    }

    define void @use_win() {
    entry:
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0,
                        i8* @win)
      ret void
    }

    define i32 @main(i8* %comm, i1 %cond) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      br i1 %cond, label %free_path, label %use_path

    free_path:
      call void @free_win()
      br label %join

    use_path:
      call void @use_win()
      br label %join

    join:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().use_after_free_windows.empty());
  bool saw_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_rma_use_after_free_order_unresolved") {
      saw_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_gap);
}

TEST_F(MPIAnalysisTest, RMAEpochRelationFlowsIntoDiagnostics) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 0, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 0, i64 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_unlock(i32 0, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_epoch_relation = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.relation.kind ==
            concurrency::RelationKind::SameSynchronizationEpoch &&
        diag.relation.reason == "mpi_rma_lock_epoch") {
      saw_epoch_relation = true;
      break;
    }
  }
  EXPECT_TRUE(saw_epoch_relation);
}

TEST_F(MPIAnalysisTest, DoubleWindowFreeIsReported) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_free(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      call i32 @MPI_Win_free(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().double_window_free.size(), 1u);
}

