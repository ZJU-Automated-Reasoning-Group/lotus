#include "MPIAnalysisTestCommon.h"

TEST_F(MPIAnalysisTest, RankRestrictedCollectivesUseDistinctProtocolSlots) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %rankv = load i32, i32* %rank, align 4
      %in_low_half = icmp slt i32 %rankv, 2
      br i1 %in_low_half, label %low, label %high

    low:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %join

    high:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %join

    join:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 2u);
  EXPECT_EQ(collectives[0].communicator_subgroup_id, 0u);
  EXPECT_EQ(collectives[1].communicator_subgroup_id, 0u);
  EXPECT_NE(collectives[0].participant_class_id, 0u);
  EXPECT_NE(collectives[1].participant_class_id, 0u);
  EXPECT_NE(collectives[0].participant_class_id,
            collectives[1].participant_class_id);
  EXPECT_EQ(collectives[0].protocol_sequence_id, 0u);
  EXPECT_EQ(collectives[1].protocol_sequence_id, 1u);
}

TEST_F(MPIAnalysisTest, SameFunctionRankPartitionedPointToPointStillMatches) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %rankv = load i32, i32* %rank, align 4
      %is_zero = icmp eq i32 %rankv, 0
      br i1 %is_zero, label %send, label %check_recv

    send:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      br label %join

    check_recv:
      %is_one = icmp eq i32 %rankv, 1
      br i1 %is_one, label %recv, label %join

    recv:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      br label %join

    join:
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
  EXPECT_TRUE(
      analysis.getProcessModel().canCommunicate(sends.front(), recvs.front()));
  EXPECT_NE(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
  EXPECT_FALSE(analysis.getResults().channel_obligations.empty());
}

TEST_F(MPIAnalysisTest, FlushMarksTrackedRMACompletion) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Get(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)
    @win = global i8 0, align 1

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 8, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Get(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush(i32 1, i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().unsynchronized_rma.empty());
  EXPECT_FALSE(analysis.getResults().rma_synchronization_facts.empty());
  EXPECT_TRUE(analysis.getResults().rma_races.empty());
}

TEST_F(MPIAnalysisTest, WaitsomeWithoutRecoverableIndicesKeepsRequestsPending) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitsome(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %indices = alloca [2 x i32], align 4
      %idx0 = getelementptr inbounds [2 x i32], [2 x i32]* %indices, i64 0, i64 0

      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8

      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitsome(i32 2, i8** %slot0, i32* null, i32* %idx0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  size_t may_complete = 0;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.request_state == RequestCompletionState::MayComplete) {
      ++may_complete;
    }
  }
  EXPECT_EQ(may_complete, 2u);
}

TEST_F(MPIAnalysisTest, WaitsomeCompletesOnlyRecoveredIndices) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitsome(i32, i8**, i32*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %indices = alloca [1 x i32], align 4
      %idx0 = getelementptr inbounds [1 x i32], [1 x i32]* %indices, i64 0, i64 0

      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      store i32 1, i32* %idx0, align 4

      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitsome(i32 2, i8** %slot0, i32* null, i32* %idx0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, SendrecvExtractionUsesCorrectOperands) {
  const char *source = R"(
    declare i32 @MPI_Sendrecv(i8*, i32, i32, i32, i32,
                              i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Sendrecv(i8* null, i32 1, i32 0, i32 7, i32 11,
                             i8* null, i32 1, i32 0, i32 3, i32 13, i8* %comm, i8* null)
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
  EXPECT_EQ(sends.front().dest_rank, 7);
  EXPECT_EQ(sends.front().tag, 11);
  EXPECT_EQ(recvs.front().source_rank, 3);
  EXPECT_EQ(recvs.front().tag, 13);
  EXPECT_EQ(sends.front().communicator, recvs.front().communicator);
}

TEST_F(MPIAnalysisTest, WildcardSourceAndTagSupportMinusTwoSentinel) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 3, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -2, i32 -2, i8* %comm, i8* null)
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
  EXPECT_TRUE(
      analysis.getProcessModel().canCommunicate(sends.front(), recvs.front()));
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::MayMatch);
}

TEST_F(MPIAnalysisTest, CollectivesComparedPerCommunicatorAndSequenceSlot) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm_a, i8* %comm_b) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm_a)
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm_b)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm_a)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().mismatched_collectives.empty());
}

TEST_F(MPIAnalysisTest,
       CommunicatorDupReusesCanonicalIdentityWithoutFalseMismatch) {
  const char *source = R"(
    declare i32 @MPI_Comm_dup(i8*, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_dup(i8* %comm, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %dup_loaded)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().mismatched_collectives.empty());
}

TEST_F(MPIAnalysisTest, UnknownDistinctCommunicatorsDoNotForceDeadlockProof) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %a = call i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b)
      %b = call i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().potential_deadlocks.empty());
}

TEST_F(MPIAnalysisTest,
       AmbiguousPointToPointCommunicatorDoesNotDisproveCommunication) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 9, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %a = call i32 @rank0(i1 %cond, i8* %comm_a, i8* %comm_b)
      %b = call i32 @rank1(i1 %cond, i8* %comm_a, i8* %comm_b)
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

  EXPECT_NE(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
}

TEST_F(MPIAnalysisTest,
       AmbiguousPointToPointCommunicatorEmitsModelGapAndKeepsChannel) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @helper_send(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 5, i8* %comm)
      ret i32 0
    }

    define i32 @helper_recv(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %comm = select i1 %cond, i8* %comm_a, i8* %comm_b
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 5, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i1 %cond, i8* %comm_a, i8* %comm_b) {
    entry:
      %a = call i32 @helper_send(i1 %cond, i8* %comm_a, i8* %comm_b)
      %b = call i32 @helper_recv(i1 %cond, i8* %comm_a, i8* %comm_b)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_model_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_channel_identity_unresolved") {
      saw_model_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_model_gap);

  bool saw_channel_relation = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_channel_identity_unresolved") {
      saw_channel_relation = true;
      break;
    }
  }
  EXPECT_TRUE(saw_channel_relation);
}

TEST_F(MPIAnalysisTest, CollectiveMatchingUsesPerCommunicatorSequenceSlots) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm)
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

TEST_F(MPIAnalysisTest, CollectiveProtocolAutomatonTracksParticipantSlots) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm)
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

  const auto &automata = analysis.getCollectiveAnalysis().getProtocolAutomata();
  ASSERT_EQ(automata.size(), 1u);
  const auto &automaton = automata.begin()->second;
  EXPECT_EQ(automaton.participant_slots.size(), 1u);
  EXPECT_EQ(automaton.slots.size(), 4u);
  EXPECT_EQ(automaton.slots.at(0).expected_type, ThreadAPI::TD_MPI_BCAST);
}

TEST_F(MPIAnalysisTest, RankGuardedCollectiveIsReportedConditional) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      br i1 %is_root, label %then, label %done

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %done

    done:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().conditional_collectives.size(), 1u);
  const auto &diagnostics =
      analysis.getCollectiveAnalysis().getProtocolDiagnostics();
  auto it = diagnostics.find("collective_rank_filtered");
  ASSERT_NE(it, diagnostics.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(MPIAnalysisTest, RMAExtractionHandlesLockAllAndAtomicOps) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_flush_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Fetch_and_op(i8*, i8*, i32, i32, i64, i32, i8*)
    declare i32 @MPI_Compare_and_swap(i8*, i8*, i8*, i32, i32, i64, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock_all(i32 0, i8* %win)
      call i32 @MPI_Fetch_and_op(i8* null, i8* null, i32 2, i32 3, i64 0, i32 0, i8* %win)
      call i32 @MPI_Compare_and_swap(i8* null, i8* null, i8* null, i32 2, i32 4, i64 8, i8* %win)
      call i32 @MPI_Win_flush_all(i8* %win)
      call i32 @MPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *fetch = findOperation(ops, ThreadAPI::TD_MPI_ACCUMULATE);
  ASSERT_NE(fetch, nullptr);
  EXPECT_EQ(fetch->target_rank, 3);
  EXPECT_NE(fetch->window, nullptr);
  EXPECT_EQ(fetch->byte_length, 4);

  const MPIOperation *lock = findOperation(ops, ThreadAPI::TD_MPI_WIN_LOCK);
  const MPIOperation *flush = findOperation(ops, ThreadAPI::TD_MPI_WIN_FLUSH);
  const MPIOperation *unlock = findOperation(ops, ThreadAPI::TD_MPI_WIN_UNLOCK);
  ASSERT_NE(lock, nullptr);
  ASSERT_NE(flush, nullptr);
  ASSERT_NE(unlock, nullptr);
  EXPECT_EQ(lock->window, fetch->window);
  EXPECT_EQ(flush->window, fetch->window);
  EXPECT_EQ(unlock->window, fetch->window);
}

TEST_F(MPIAnalysisTest, PMPI_RMAVariantsPreserveWindowAndTargetExtraction) {
  const char *source = R"(
    declare i32 @PMPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @PMPI_Win_lock_all(i32, i8*)
    declare i32 @PMPI_Win_flush_all(i8*)
    declare i32 @PMPI_Win_unlock_all(i8*)
    declare i32 @PMPI_Fetch_and_op(i8*, i8*, i32, i32, i64, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @PMPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* %win)
      call i32 @PMPI_Win_lock_all(i32 0, i8* %win)
      call i32 @PMPI_Fetch_and_op(i8* null, i8* null, i32 2, i32 5, i64 8, i32 0, i8* %win)
      call i32 @PMPI_Win_flush_all(i8* %win)
      call i32 @PMPI_Win_unlock_all(i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *fetch = findOperation(ops, ThreadAPI::TD_MPI_ACCUMULATE);
  ASSERT_NE(fetch, nullptr);
  EXPECT_EQ(fetch->target_rank, 5);
  EXPECT_NE(fetch->window, nullptr);
  EXPECT_EQ(fetch->byte_length, 4);

  const MPIOperation *lock = findOperation(ops, ThreadAPI::TD_MPI_WIN_LOCK);
  const MPIOperation *flush = findOperation(ops, ThreadAPI::TD_MPI_WIN_FLUSH);
  const MPIOperation *unlock = findOperation(ops, ThreadAPI::TD_MPI_WIN_UNLOCK);
  ASSERT_NE(lock, nullptr);
  ASSERT_NE(flush, nullptr);
  ASSERT_NE(unlock, nullptr);
  EXPECT_EQ(lock->window, fetch->window);
  EXPECT_EQ(flush->window, fetch->window);
  EXPECT_EQ(unlock->window, fetch->window);
}

TEST_F(MPIAnalysisTest, WinAllocateVariantsUseCommunicatorOperandForClasses) {
  const char *source = R"(
    declare i32 @MPI_Win_allocate(i64, i32, i8*, i8*, i8*)
    declare i32 @MPI_Win_allocate_shared(i64, i32, i8*, i8*, i8*)

    define i32 @main(i8* %comm, i8* %win1, i8* %win2) {
    entry:
      call i32 @MPI_Win_allocate(i64 16, i32 4, i8* null, i8* %comm, i8* %win1)
      call i32 @MPI_Win_allocate_shared(i64 32, i32 4, i8* null, i8* %comm, i8* %win2)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  size_t checked = 0;
  for (const MPIOperation &op : ops) {
    if (op.td_type != ThreadAPI::TD_MPI_WIN_CREATE) {
      continue;
    }
    ++checked;
    EXPECT_EQ(op.communicator, module->getFunction("main")->getArg(0));
    EXPECT_NE(op.communicator_class_id, 0u);
  }
  EXPECT_EQ(checked, 2u);
}

TEST_F(MPIAnalysisTest, CommDupWithInfoUsesNewCommunicatorResultSlot) {
  const char *source = R"(
    declare i32 @MPI_Comm_dup_with_info(i8*, i8*, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i8* %info, i8* %newcomm) {
    entry:
      call i32 @MPI_Comm_dup_with_info(i8* %comm, i8* %info, i8* %newcomm)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator, module->getFunction("main")->getArg(1));
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, CommIdupProducesPendingRequestAndDerivedCommunicator) {
  const char *source = R"(
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  const auto &summary = summaries.begin()->second;
  EXPECT_EQ(summary.state, MPIRequestState::Active);
  EXPECT_EQ(summary.origin_inst, &module->getFunction("main")->getEntryBlock().front());

  const auto &ops = analysis.getProcessModel().getAllOperations();
  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator, nullptr);
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, CommIdupWaitCompletesRequestLifecycle) {
  const char *source = R"(
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define i32 @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  EXPECT_EQ(summaries.begin()->second.state, MPIRequestState::MustComplete);
}

TEST_F(MPIAnalysisTest, CommIdupTestFalseKeepsRequestPendingUntilFreed) {
  const char *source = R"(
    declare i32 @MPI_Comm_idup(i8*, i8*, i8*)
    declare i32 @MPI_Test(i8*, i32*, i8*)
    declare i32 @MPI_Request_free(i8*)

    define i32 @main(i8* %comm, i8* %newcomm, i8* %req) {
    entry:
      %flag = alloca i32, align 4
      store i32 0, i32* %flag, align 4
      call i32 @MPI_Comm_idup(i8* %comm, i8* %newcomm, i8* %req)
      call i32 @MPI_Test(i8* %req, i32* %flag, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  EXPECT_EQ(summaries.begin()->second.state, MPIRequestState::Freed);
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  auto it = deferred.find("test_unknown_flag");
  if (it != deferred.end()) {
    EXPECT_EQ(it->second, 0u);
  }
}

TEST_F(MPIAnalysisTest, CommSplitTypeDoesNotTreatSplitTypeAsColorIdentity) {
  const char *source = R"(
    declare i32 @MPI_Comm_split_type(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i8* %info, i8* %newcomm) {
    entry:
      call i32 @MPI_Comm_split_type(i8* %comm, i32 1, i32 0, i8* %info, i8* %newcomm)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_subgroup_identity_unresolved") {
      saw_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_gap);

  const auto &ops = analysis.getProcessModel().getAllOperations();
  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, TopologyCommunicatorCreationRegistersDerivedHandle) {
  const char *source = R"(
    declare i32 @MPI_Cart_create(i8*, i32, i32*, i32*, i32, i8*)
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm, i32* %dims, i32* %periods, i8* %newcomm) {
    entry:
      call i32 @MPI_Cart_create(i8* %comm, i32 1, i32* %dims, i32* %periods, i32 0, i8* %newcomm)
      call i32 @MPI_Barrier(i8* %newcomm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();
  const auto &ops = analysis.getProcessModel().getAllOperations();

  const MPIOperation *barrier = findOperation(ops, ThreadAPI::TD_MPI_BARRIER);
  ASSERT_NE(barrier, nullptr);
  EXPECT_NE(barrier->communicator_class_id, 0u);
}

TEST_F(MPIAnalysisTest, OpenMPIInternalCollectiveNamesAreRecognized) {
  const char *source = R"(
    declare i32 @ompi_mpi_bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @ompi_mpi_bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING), 1u);
}

TEST_F(MPIAnalysisTest, SemanticDescriptorCoverageForCoreMPITypes) {
  const std::vector<ThreadAPI::TD_TYPE> required_types = {
      ThreadAPI::TD_MPI_INIT,
      ThreadAPI::TD_MPI_FINALIZE,
      ThreadAPI::TD_MPI_SEND,
      ThreadAPI::TD_MPI_RECV,
      ThreadAPI::TD_MPI_SENDRECV,
      ThreadAPI::TD_MPI_ISEND,
      ThreadAPI::TD_MPI_IRECV,
      ThreadAPI::TD_MPI_WAIT,
      ThreadAPI::TD_MPI_TEST,
      ThreadAPI::TD_MPI_BARRIER,
      ThreadAPI::TD_MPI_BCAST,
      ThreadAPI::TD_MPI_REDUCE,
      ThreadAPI::TD_MPI_WIN_CREATE,
      ThreadAPI::TD_MPI_PUT,
      ThreadAPI::TD_MPI_WIN_LOCK,
      ThreadAPI::TD_MPI_COMM_DUP,
      ThreadAPI::TD_MPI_COMM_CREATE,
      ThreadAPI::TD_MPI_REQUEST_FREE,
      ThreadAPI::TD_MPI_TYPE_CONTIGUOUS,
      ThreadAPI::TD_MPI_TYPE_COMMIT,
      ThreadAPI::TD_MPI_SESSION_INIT,
      ThreadAPI::TD_MPI_SESSION_FINALIZE,
  };

  for (ThreadAPI::TD_TYPE type : required_types) {
    const MPISemanticDescriptor *descriptor = lookupMPISemantic(type);
    ASSERT_NE(descriptor, nullptr);
    if (type == ThreadAPI::TD_MPI_SEND || type == ThreadAPI::TD_MPI_RECV ||
        type == ThreadAPI::TD_MPI_ISEND || type == ThreadAPI::TD_MPI_IRECV) {
      EXPECT_NE(descriptor->communicator_arg, -1);
      EXPECT_NE(descriptor->count_arg, -1);
      EXPECT_NE(descriptor->datatype_arg, -1);
      EXPECT_NE(descriptor->peer_rank_arg, -1);
      EXPECT_NE(descriptor->tag_arg, -1);
    }
    if (type == ThreadAPI::TD_MPI_WAIT || type == ThreadAPI::TD_MPI_WAITALL ||
        type == ThreadAPI::TD_MPI_TEST || type == ThreadAPI::TD_MPI_TESTALL) {
      EXPECT_NE(descriptor->request_arg, -1);
    }
  }
}

TEST_F(MPIAnalysisTest,
       SemanticDescriptorCoverageForNewStatusAndTopologyTypes) {
  const MPISemanticDescriptor *get_count =
      lookupMPISemantic(ThreadAPI::TD_MPI_GET_COUNT);
  ASSERT_NE(get_count, nullptr);
  EXPECT_EQ(get_count->kind, MPIOpKind::REQUEST_MANAGEMENT);
  EXPECT_EQ(get_count->family, MPISemanticFamily::Request);

  const MPISemanticDescriptor *status_set =
      lookupMPISemantic(ThreadAPI::TD_MPI_STATUS_SET_ELEMENTS);
  ASSERT_NE(status_set, nullptr);
  EXPECT_EQ(status_set->kind, MPIOpKind::REQUEST_MANAGEMENT);
  EXPECT_EQ(status_set->family, MPISemanticFamily::Request);

  const MPISemanticDescriptor *cart_create =
      lookupMPISemantic(ThreadAPI::TD_MPI_CART_CREATE);
  ASSERT_NE(cart_create, nullptr);
  EXPECT_EQ(cart_create->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(cart_create->family, MPISemanticFamily::Communicator);

  const MPISemanticDescriptor *dist_graph_neighbors =
      lookupMPISemantic(ThreadAPI::TD_MPI_DIST_GRAPH_NEIGHBORS);
  ASSERT_NE(dist_graph_neighbors, nullptr);
  EXPECT_EQ(dist_graph_neighbors->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(dist_graph_neighbors->family, MPISemanticFamily::Communicator);

  const MPISemanticDescriptor *graph_map =
      lookupMPISemantic(ThreadAPI::TD_MPI_GRAPH_MAP);
  ASSERT_NE(graph_map, nullptr);
  EXPECT_EQ(graph_map->kind, MPIOpKind::COMM_MANAGEMENT);
  EXPECT_EQ(graph_map->family, MPISemanticFamily::Communicator);
}

TEST_F(MPIAnalysisTest, MPISpecDescriptorsAreExplicitlyCovered) {
  std::ifstream spec(
      "/Users/rainoftime/Work/analysis/lotus/config/mpi_api.spec");
  ASSERT_TRUE(spec.is_open());

  auto isExplicitlyIgnored = [](const std::string &type_name) {
    return type_name.find("ERRHANDLER") != std::string::npos ||
           type_name.find("INFO_") != std::string::npos ||
           type_name.find("STATUS_") != std::string::npos ||
           type_name.find("ERROR_") != std::string::npos ||
           type_name.find("GRAPH_") != std::string::npos ||
           type_name.find("CART_") != std::string::npos ||
           type_name.find("DIST_GRAPH_") != std::string::npos ||
           type_name.find("GET_COUNT") != std::string::npos ||
           type_name.find("GET_ELEMENTS") != std::string::npos;
  };

  std::string line;
  size_t checked = 0;
  while (std::getline(spec, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream iss(line);
    std::string symbol;
    std::string type_name;
    if (!(iss >> symbol >> type_name)) {
      continue;
    }
    if (type_name.find("TD_MPI_") != 0) {
      continue;
    }

    LLVMContext local_context;
    Module local_module("mpi_spec_probe", local_context);
    auto *fn_ty = FunctionType::get(Type::getInt32Ty(local_context), false);
    Function *fn = Function::Create(fn_ty, Function::ExternalLinkage, symbol,
                                    local_module);
    ThreadAPI::TD_TYPE type = ThreadAPI::getThreadAPI()->getType(fn);
    if (type == ThreadAPI::TD_DUMMY) {
      continue;
    }
    if (isExplicitlyIgnored(type_name)) {
      continue;
    }

    EXPECT_NE(lookupMPISemantic(type), nullptr)
        << "missing descriptor for " << symbol << " " << type_name;
    ++checked;
  }

  EXPECT_GT(checked, 50u);
}

TEST_F(MPIAnalysisTest, NeighborCollectivesUseDistinctProtocolClass) {
  const char *source = R"(
    declare i32 @MPI_Alltoall(i8*, i32, i32, i8*, i32, i32, i8*)
    declare i32 @MPI_Neighbor_alltoall(i8*, i32, i32, i8*, i32, i32, i8*)

    define void @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Alltoall(i8* null, i32 1, i32 2, i8* null, i32 1, i32 2, i8* %comm)
      ret void
    }

    define void @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Neighbor_alltoall(i8* null, i32 4, i32 2, i8* null, i32 4, i32 2, i8* %comm)
      ret void
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 0u);
}

TEST_F(MPIAnalysisTest, IntercommunicatorCreateGetsDedicatedOperationKind) {
  const char *source = R"(
    declare i32 @MPI_Intercomm_create(i8*, i32, i8*, i32, i32, i8**)

    define i32 @main(i8* %local, i8* %peer) {
    entry:
      %out = alloca i8*, align 8
      call i32 @MPI_Intercomm_create(i8* %local, i32 0, i8* %peer, i32 0, i32 7, i8** %out)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::INTERCOMM_CREATION), 1u);
}

TEST_F(MPIAnalysisTest, DerivedDatatypeExtentPropagatesToPointToPointOps) {
  const char *source = R"(
    declare i32 @MPI_Type_contiguous(i32, i32, i8*)
    declare i32 @MPI_Send(i8*, i32, i8*, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %derived_type = alloca i8, align 1
      call i32 @MPI_Type_contiguous(i32 4, i32 2, i8* %derived_type)
      call i32 @MPI_Send(i8* null, i32 3, i8* %derived_type, i32 1, i32 7, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  EXPECT_EQ(sends.front().datatype_size, 16);
  EXPECT_EQ(sends.front().byte_length, 48);
}

TEST_F(MPIAnalysisTest, WrappedCommRankStillRefinesConditionalCollectives) {
  const char *source = R"(
    declare i32 @__wrap_MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @__wrap_MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      br i1 %is_root, label %then, label %done

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %done

    done:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().conditional_collectives.size(), 1u);
}

