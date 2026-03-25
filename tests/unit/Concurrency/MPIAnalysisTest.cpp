#include "MPIAnalysisTestCommon.h"

TEST_F(MPIAnalysisTest, SendRecvCreatesSendAndReceiveOperations) {
  const char *source = R"(
    declare i32 @MPI_Sendrecv(i8*, i32, i32, i32, i32,
                              i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Sendrecv(i8* null, i32 1, i32 0, i32 1, i32 7,
                             i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getProcessModel()
                .getOperationsByKind(MPIOpKind::SEND_BLOCKING)
                .size(),
            1u);
  EXPECT_EQ(analysis.getProcessModel()
                .getOperationsByKind(MPIOpKind::RECV_BLOCKING)
                .size(),
            1u);
}

TEST_F(MPIAnalysisTest, RankIncompatiblePointToPointDoesNotMatch) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %rankv = load i32, i32* %rank, align 4
      %is_one = icmp eq i32 %rankv, 1
      br i1 %is_one, label %send, label %check_recv

    send:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      br label %join

    check_recv:
      %is_zero = icmp eq i32 %rankv, 0
      br i1 %is_zero, label %recv, label %join

    recv:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 2, i32 7, i8* %comm, i8* null)
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
  EXPECT_FALSE(
      analysis.getProcessModel().canCommunicate(sends.front(), recvs.front()));
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
}

TEST_F(MPIAnalysisTest, WaitAllCompletesOnlyListedRequests) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [1 x i8*], align 8
      %slot0 = getelementptr inbounds [1 x i8*], [1 x i8*]* %reqs, i64 0, i64 0
      store i8* %req1, i8** %slot0, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitall(i32 1, i8** %slot0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, IprobeDoesNotEnterRequestLifecycle) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Iprobe(i32, i32, i8*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      %flag = alloca i32, align 4
      %status = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Iprobe(i32 1, i32 7, i8* %comm, i32* %flag, i8* %status)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, MatchedMessageOperationsUseReceiveSemantics) {
  const char *source = R"(
    declare i32 @MPI_Mprobe(i32, i32, i8*, i8*, i8*)
    declare i32 @MPI_Improbe(i32, i32, i8*, i32*, i8*, i8*)
    declare i32 @MPI_Mrecv(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Imrecv(i8*, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %flag = alloca i32, align 4
      %msg1 = alloca i8, align 1
      %msg2 = alloca i8, align 1
      %status = alloca i8, align 1
      %req = alloca i8, align 1
      call i32 @MPI_Mprobe(i32 1, i32 7, i8* %comm, i8* %msg1, i8* %status)
      call i32 @MPI_Improbe(i32 1, i32 7, i8* %comm, i32* %flag, i8* %msg2, i8* %status)
      call i32 @MPI_Mrecv(i8* null, i32 4, i32 0, i8* %msg1, i8* %status)
      call i32 @MPI_Imrecv(i8* null, i32 4, i32 0, i8* %msg2, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &ops = analysis.getProcessModel().getAllOperations();
  auto findOp = [&](auto predicate) -> const MPIOperation * {
    for (const auto &op : ops) {
      if (predicate(op)) {
        return &op;
      }
    }
    return nullptr;
  };
  const MPIOperation *mprobe = findOp([](const MPIOperation &op) {
    return op.kind == MPIOpKind::PROBE_BLOCKING;
  });
  const MPIOperation *improbe = findOp([](const MPIOperation &op) {
    return op.td_type == ThreadAPI::TD_MPI_IMPROBE;
  });
  const MPIOperation *mrecv = findOp([](const MPIOperation &op) {
    return op.td_type == ThreadAPI::TD_MPI_MRECV;
  });
  const MPIOperation *imrecv = findOp([](const MPIOperation &op) {
    return op.td_type == ThreadAPI::TD_MPI_IMRECV;
  });
  ASSERT_NE(mprobe, nullptr);
  ASSERT_NE(improbe, nullptr);
  ASSERT_NE(mrecv, nullptr);
  ASSERT_NE(imrecv, nullptr);

  EXPECT_EQ(mprobe->kind, MPIOpKind::PROBE_BLOCKING);
  EXPECT_EQ(mprobe->blocking_mode, MPIBlockingMode::Blocking);
  EXPECT_EQ(improbe->kind, MPIOpKind::PROBE_NONBLOCKING);
  EXPECT_EQ(improbe->blocking_mode, MPIBlockingMode::NonBlocking);

  EXPECT_EQ(mrecv->kind, MPIOpKind::RECV_BLOCKING);
  EXPECT_EQ(mrecv->blocking_mode, MPIBlockingMode::Blocking);
  EXPECT_TRUE(mrecv->matched_message);
  EXPECT_EQ(imrecv->kind, MPIOpKind::RECV_NONBLOCKING);
  EXPECT_EQ(imrecv->blocking_mode, MPIBlockingMode::NonBlocking);
  EXPECT_TRUE(imrecv->matched_message);
  EXPECT_NE(imrecv->request, nullptr);
}

TEST_F(MPIAnalysisTest, BitcastedMPICallStillLowersIntoOperation) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %bar = call i32 bitcast (i32 (i8*)* @MPI_Barrier to i32 (i8*)*)(i8* %comm)
      ret i32 %bar
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getOperationCount(MPIOpKind::BARRIER_BLOCKING), 1u);
}

TEST_F(MPIAnalysisTest, ReceiveAnyTagIsNotReportedAsInvalidTag) {
  const char *source = R"(
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %status = alloca i8, align 1
      %recv = call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 -1,
                                 i8* %comm, i8* %status)
      ret i32 %recv
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().invalid_tags.empty());
}

TEST_F(MPIAnalysisTest, TestWithFalseFlagKeepsRequestPending) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Test(i8*, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      %flag = alloca i32, align 4
      store i32 0, i32* %flag, align 4
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Test(i8* %req, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
}

TEST_F(MPIAnalysisTest, TestanyWithoutRecoverableIndexDoesNotCompleteRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Testany(i32, i8**, i32*, i32*, i8*)

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
      call i32 @MPI_Testany(i32 2, i8** %slot0, i32* %index, i32* %flag, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  auto it = deferred.find("testany_unknown_index");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
  size_t may_complete = 0;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.request_state == RequestCompletionState::MayComplete) {
      ++may_complete;
    }
  }
  EXPECT_EQ(may_complete, 2u);
}

TEST_F(MPIAnalysisTest, WaitanyWithoutRecoverableIndexDoesNotCompleteRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitany(i32, i8**, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %index = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitany(i32 2, i8** %slot0, i32* %index, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
  const auto &deferred = analysis.getProcessModel().getDeferredLoweringStats();
  auto it = deferred.find("waitany_unknown_index");
  ASSERT_NE(it, deferred.end());
  EXPECT_GT(it->second, 0u);
}

TEST_F(MPIAnalysisTest, NonBlockingCollectiveRequestCompletesThroughWait) {
  const char *source = R"(
    declare i32 @MPI_Ibcast(i8*, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibcast(i8* null, i32 1, i32 0, i32 0, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, RequestFreeTerminatesOutstandingRequest) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      call i32 @MPI_Request_free(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, NonblockingCollectiveUsesSingleRequestArity) {
  const char *source = R"(
    declare i32 @MPI_Ibarrier(i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Ibarrier(i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_sets = analysis.getRequestSetFacts();
  auto issued = std::find_if(
      request_sets.begin(), request_sets.end(),
      [](const MPIRequestSetFact &fact) {
        return fact.provenance == "mpi_request_set_issue";
      });
  ASSERT_NE(issued, request_sets.end());
  EXPECT_EQ(issued->arity, MPIRequestArity::Single);
  EXPECT_EQ(issued->kind, MPIRequestSetKind::Collective);
}

TEST_F(MPIAnalysisTest, SemanticEventsCaptureCollectiveAndRequestSemantics) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_request_issue = false;
  bool saw_request_completion = false;
  bool saw_collective = false;
  for (const auto &event : analysis.getProcessModel().getSemanticEvents()) {
    if (event.request.action == MPIRequestActionKind::IssueNonBlocking) {
      saw_request_issue = true;
    }
    if (event.request.action == MPIRequestActionKind::CompleteMust) {
      saw_request_completion = true;
    }
    if (event.has_collective_semantics &&
        event.collective.type == ThreadAPI::TD_MPI_BCAST &&
        event.collective.count == 1 && event.collective.root_rank == 0) {
      saw_collective = true;
    }
  }

  EXPECT_TRUE(saw_request_issue);
  EXPECT_TRUE(saw_request_completion);
  EXPECT_TRUE(saw_collective);
}

TEST_F(MPIAnalysisTest, SemanticEventsCapturePointToPointObligations) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_send_event = false;
  bool saw_recv_event = false;
  for (const auto &event : analysis.getProcessModel().getSemanticEvents()) {
    if (!event.has_point_to_point_semantics) {
      continue;
    }
    saw_send_event = saw_send_event || event.point_to_point.is_send;
    saw_recv_event = saw_recv_event || event.point_to_point.is_recv;
  }

  const auto &obligations =
      analysis.getProcessModel().getPointToPointObligations();
  ASSERT_EQ(obligations.size(), 1u);
  EXPECT_TRUE(saw_send_event);
  EXPECT_TRUE(saw_recv_event);
  EXPECT_EQ(obligations.front().proof, MPIMatchProofKind::MustMatch);
  EXPECT_EQ(obligations.front().relation.proof,
            concurrency::ProofStrength::Must);
}

TEST_F(MPIAnalysisTest, SemanticEventsCaptureRMAEpochFacts) {
  const char *source = R"(
    declare i32 @MPI_Win_create(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Win_create(i8* null, i32 8, i32 4, i8* %comm, i8* %win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i32 0, i32 1, i32 0, i8* %win)
      call i32 @MPI_Win_flush(i32 1, i8* %win)
      call i32 @MPI_Win_unlock(i32 1, i8* %win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_epoch = false;
  bool saw_flush_completion = false;
  for (const auto &event : analysis.getProcessModel().getSemanticEvents()) {
    if (!event.has_rma_semantics || !event.rma.is_data_operation) {
      continue;
    }
    saw_epoch = event.rma.epoch_id != 0 &&
                event.rma.sync_model == MPIRMASyncModel::LockUnlock;
    saw_flush_completion = event.rma.epoch_completion ==
                           MPIRMAEpochCompletionKind::RemoteGuaranteed;
  }

  EXPECT_TRUE(saw_epoch);
  EXPECT_TRUE(saw_flush_completion);
}

TEST_F(MPIAnalysisTest, StartedPersistentRequestWithoutCompletionIsOrphaned) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_EQ(analysis.getResults().orphaned_requests.size(), 1u);
  EXPECT_NE(analysis.getResults().orphaned_requests.front().issue_inst,
            nullptr);
}

TEST_F(MPIAnalysisTest, StartedPersistentRequestCompletesThroughWait) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, RequestStateDomainTracksPersistentLifecycleHistory) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Start(i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Request_free(i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Start(i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Request_free(i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getProcessModel().getRequestStateSummaries();
  ASSERT_EQ(summaries.size(), 1u);
  const auto &summary = summaries.begin()->second;
  EXPECT_TRUE(summary.is_persistent);
  EXPECT_EQ(summary.state, MPIRequestState::Freed);
  ASSERT_EQ(summary.history.size(), 4u);
  EXPECT_EQ(summary.history[0].action, MPIRequestActionKind::CreatePersistent);
  EXPECT_EQ(summary.history[1].action,
            MPIRequestActionKind::ActivatePersistent);
  EXPECT_EQ(summary.history[2].action, MPIRequestActionKind::CompleteMust);
  EXPECT_EQ(summary.history[3].action, MPIRequestActionKind::Free);
}

TEST_F(MPIAnalysisTest, StartallActivatesPersistentRequestArrays) {
  const char *source = R"(
    declare i32 @MPI_Send_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv_init(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Startall(i32, i8**)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Send_init(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Recv_init(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Startall(i32 2, i8** %slot0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().orphaned_requests.size(), 2u);
  const auto &request_sets = analysis.getRequestSetFacts();
  auto activation = std::find_if(
      request_sets.begin(), request_sets.end(),
      [](const MPIRequestSetFact &fact) {
        return fact.provenance == "mpi_request_set_activate";
      });
  ASSERT_NE(activation, request_sets.end());
  EXPECT_EQ(activation->arity, MPIRequestArity::Array);
  EXPECT_EQ(activation->completion_scope,
            MPIRequestCompletionScopeKind::AllOfSet);
}

TEST_F(MPIAnalysisTest,
       UnresolvedRequestArrayDoesNotCreateSyntheticSingletonRequest) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)

    define i32 @main(i8* %comm, i1 %cond) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [1 x i8*], align 8
      %slot0 = getelementptr inbounds [1 x i8*], [1 x i8*]* %reqs, i64 0, i64 0
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 2, i32 8, i8* %comm, i8* %req2)
      br i1 %cond, label %lhs, label %rhs

    lhs:
      store i8* %req1, i8** %slot0, align 8
      br label %join

    rhs:
      store i8* %req2, i8** %slot0, align 8
      br label %join

    join:
      call i32 @MPI_Waitall(i32 1, i8** %slot0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getRequestSetFacts().size(), 2u);
  bool saw_storage_gap = false;
  for (const MPIModelGap &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_request_storage_escaped") {
      saw_storage_gap = true;
      break;
    }
  }
  EXPECT_TRUE(saw_storage_gap);
}

TEST_F(MPIAnalysisTest,
       PointToPointDoesNotMatchAcrossDisjointSplitCommunicators) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 0, i32 0, i8** %split)
      %sub = load i8*, i8** %split, align 8
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %sub)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      %status = alloca i8, align 1
      call i32 @MPI_Comm_split(i8* %comm, i32 1, i32 0, i8** %split)
      %sub = load i8*, i8** %split, align 8
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %sub, i8* %status)
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

  auto sends =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::SEND_BLOCKING);
  auto recvs =
      analysis.getProcessModel().getOperationsByKind(MPIOpKind::RECV_BLOCKING);
  ASSERT_EQ(sends.size(), 1u);
  ASSERT_EQ(recvs.size(), 1u);
  EXPECT_NE(sends.front().communicator_subgroup_id, 0u);
  EXPECT_NE(recvs.front().communicator_subgroup_id, 0u);
  EXPECT_NE(sends.front().communicator_subgroup_id,
            recvs.front().communicator_subgroup_id);
  EXPECT_EQ(analysis.getProcessModel().classifyCommunicationMatch(
                sends.front(), recvs.front()),
            MPICommunicationMatch::NoMatch);
}

TEST_F(MPIAnalysisTest, CancelTerminatesOutstandingRequest) {
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

  EXPECT_TRUE(analysis.getResults().orphaned_requests.empty());
}

TEST_F(MPIAnalysisTest, PrintResultsIncludesDetailedCounters) {
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

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::string output;
  raw_string_ostream os(output);
  analysis.printResults(os);
  os.flush();

  EXPECT_NE(output.find("MPI init/finalize ops: 0/0"), std::string::npos);
  EXPECT_NE(output.find("Blocking point-to-point ops: 0"), std::string::npos);
  EXPECT_NE(output.find("Non-blocking MPI operations: 3"), std::string::npos);
  EXPECT_NE(output.find("Non-blocking point-to-point ops: 2"),
            std::string::npos);
  EXPECT_NE(output.find("Wait/Test ops: 0/1"), std::string::npos);
  EXPECT_NE(output.find("RMA window lifecycle ops: 1"), std::string::npos);
  EXPECT_NE(output.find("Collective partial-reachability observations: 0"),
            std::string::npos);
  EXPECT_NE(output.find("Requests with may-complete status: 1"),
            std::string::npos);
  EXPECT_NE(output.find("Requests with terminal status: 1"), std::string::npos);
  EXPECT_NE(output.find("Requests with freed status: 1"), std::string::npos);
  EXPECT_NE(
      output.find(
          "Normalization confidence (exact/pmpi/openmpi-forwarder/unknown):"),
      std::string::npos);
  EXPECT_NE(output.find("Deferred MPI semantic lowering total: 1"),
            std::string::npos);
}

