#include "MPIAnalysisTestCommon.h"

TEST_F(MPIAnalysisTest, CommunicatorSplitSharesStableSubgroupAcrossRanks) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 7, i32 0, i8** %split)
      %split_loaded = load i8*, i8** %split, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %split_loaded)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 7, i32 1, i8** %split)
      %split_loaded = load i8*, i8** %split, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %split_loaded)
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

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 2u);
  EXPECT_NE(collectives[0].communicator_subgroup_id, 0u);
  EXPECT_NE(collectives[1].communicator_subgroup_id, 0u);
  EXPECT_TRUE(analysis.getResults().mismatched_collectives.empty());
}

TEST_F(MPIAnalysisTest, UnknownSplitColorProducesExplicitSubgroupModelGap) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm, i32 %color) {
    entry:
      %split = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %comm, i32 %color, i32 0, i8** %split)
      %split_loaded = load i8*, i8** %split, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %split_loaded)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_subgroup_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_subgroup_identity_unresolved") {
      saw_subgroup_gap = true;
      EXPECT_NE(gap.subgroup_id, 0u);
    }
  }
  EXPECT_TRUE(saw_subgroup_gap);

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 1u);
  EXPECT_NE(collectives[0].communicator_subgroup_id, 0u);
}

TEST_F(MPIAnalysisTest, CommunicatorDupPreservesSplitSubgroupFacts) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Comm_dup(i8*, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %world) {
    entry:
      %sub = alloca i8*, align 8
      %dup = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %world, i32 0, i32 7, i8** %sub)
      %child = load i8*, i8** %sub, align 8
      call i32 @MPI_Comm_dup(i8* %child, i8** %dup)
      %dup_loaded = load i8*, i8** %dup, align 8
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %child)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %dup_loaded)
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
  EXPECT_EQ(collectives[0].communicator_class_id, collectives[1].communicator_class_id);
  EXPECT_EQ(collectives[0].communicator_subgroup_id,
            collectives[1].communicator_subgroup_id);
}

TEST_F(MPIAnalysisTest, UnrelatedCommunicatorArgumentsDoNotCollapseByPosition) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @left(i8* %comm_left) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm_left)
      ret void
    }

    define void @right(i8* %comm_right) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm_right)
      ret void
    }

    define i32 @main(i8* %comm0, i8* %comm1) {
    entry:
      call void @left(i8* %comm0)
      call void @right(i8* %comm1)
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
  EXPECT_NE(collectives[0].communicator_class_id, 0u);
  EXPECT_NE(collectives[1].communicator_class_id, 0u);
  EXPECT_NE(collectives[0].communicator_class_id,
            collectives[1].communicator_class_id);
}

TEST_F(MPIAnalysisTest,
       AmbiguousHelperCommunicatorDoesNotCollapseAndEmitsModelGap) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @helper(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm0, i8* %comm1) {
    entry:
      call void @helper(i8* %comm0)
      call void @helper(i8* %comm1)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 1u);
  EXPECT_EQ(collectives[0].communicator_class_id, 0u);

  size_t gap_count = 0;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_communicator_identity_ambiguous") {
      ++gap_count;
    }
  }
  EXPECT_GE(gap_count, 1u);
}

TEST_F(MPIAnalysisTest, HelperCommunicatorReusedFromSameRootStillUnifies) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @helper(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm0) {
    entry:
      call void @helper(i8* %comm0)
      call void @helper(i8* %comm0)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  auto collectives = analysis.getProcessModel().getOperationsByKind(
      MPIOpKind::COLLECTIVE_BLOCKING);
  ASSERT_EQ(collectives.size(), 1u);
  EXPECT_NE(collectives[0].communicator_class_id, 0u);
  bool saw_ambiguous_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_communicator_identity_ambiguous") {
      saw_ambiguous_gap = true;
      break;
    }
  }
  EXPECT_FALSE(saw_ambiguous_gap);
}

TEST_F(MPIAnalysisTest, LoadedCommunicatorStillParticipatesInCollectiveMismatchChecks) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Reduce(i8*, i8*, i32, i32, i32, i32, i8*)

    define void @rank0(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define void @rank1(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Reduce(i8* null, i8* null, i32 1, i32 0, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %slot = alloca i8*
      store i8* %comm, i8** %slot
      call void @rank0(i8** %slot)
      call void @rank1(i8** %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().mismatched_collectives.size(), 1u);
}

TEST_F(MPIAnalysisTest, LoadedCommunicatorStillParticipatesInWrongRootChecks) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @rank0(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define void @rank1(i8** %comm_slot) {
    entry:
      %comm = load i8*, i8** %comm_slot
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 1, i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %slot = alloca i8*
      store i8* %comm, i8** %slot
      call void @rank0(i8** %slot)
      call void @rank1(i8** %slot)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_EQ(analysis.getResults().wrong_root_ranks.size(), 1u);
}

TEST_F(MPIAnalysisTest, NonWorldCommunicatorIsNotMarkedAsWorldByClassOrder) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  for (const auto &fact : analysis.getCommunicatorFacts()) {
    EXPECT_NE(fact.creation_kind, MPICommunicatorCreationKind::World);
  }
}

TEST_F(MPIAnalysisTest, KnownWorldHandleStillMapsToWorldFact) {
  const char *source = R"(
    @MPI_COMM_WORLD = external global i8
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main() {
    entry:
      %world = load i8, i8* @MPI_COMM_WORLD, align 1
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0,
                          i8* @MPI_COMM_WORLD)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_world = false;
  for (const auto &fact : analysis.getCommunicatorFacts()) {
    if (fact.creation_kind == MPICommunicatorCreationKind::World) {
      saw_world = true;
    }
  }
  EXPECT_TRUE(saw_world);
}

TEST_F(MPIAnalysisTest, WildcardReceiveWithKnownCommunicatorRemainsMayMatch) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 -1, i8* %comm, i8* null)
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

TEST_F(MPIAnalysisTest, FlushLocalDoesNotSuppressPotentialRaces) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_flush_local_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush_local_all(i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush_local_all(i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
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

  EXPECT_FALSE(analysis.getResults().rma_races.empty());
}

TEST_F(MPIAnalysisTest, ThreeRankBlockingCycleIsDetected) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 1, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 2, i32 3, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 2, i32 2, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 1, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @rank2(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 3, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 2, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %a = call i32 @rank0(i8* %comm)
      %b = call i32 @rank1(i8* %comm)
      %c = call i32 @rank2(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_GE(analysis.getResults().potential_deadlocks.size(), 3u);
}

TEST_F(MPIAnalysisTest, SameShapedRMALockEpochsAcrossRanksStillRace) {
  const char *source = R"(
    @win = global i8 0, align 1
    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Win_unlock_all(i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
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

  EXPECT_EQ(analysis.getResults().rma_races.size(), 1u);
}

TEST_F(MPIAnalysisTest, PMPIAndOpenMPINormalizationConfidenceIsExposed) {
  const char *source = R"(
    declare i32 @PMPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @ompi_mpi_bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @PMPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      call i32 @ompi_mpi_bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_pmpi = false;
  bool saw_openmpi = false;
  for (const auto &op : analysis.getProcessModel().getAllOperations()) {
    if (op.normalization_confidence == NormalizationConfidence::PMPIWrapper) {
      saw_pmpi = true;
    }
    if (op.normalization_confidence ==
        NormalizationConfidence::KnownOpenMPIForwarder) {
      saw_openmpi = true;
    }
  }
  EXPECT_TRUE(saw_pmpi);
  EXPECT_TRUE(saw_openmpi);
}

TEST_F(MPIAnalysisTest, StructuredDiagnosticsCaptureSemanticRelationAndCode) {
  const char *source = R"(
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Gather(i8*, i32, i32, i8*, i32, i32, i32, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Gather(i8* null, i32 1, i32 0, i8* null, i32 1, i32 0, i32 0, i8* %comm)
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

  EXPECT_FALSE(analysis.getResults().diagnostics.empty());
  bool saw_protocol_code = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot" ||
        diag.code == "mpi_collective_protocol_automaton_slot") {
      saw_protocol_code = true;
      break;
    }
  }
  EXPECT_TRUE(saw_protocol_code);
}

TEST_F(MPIAnalysisTest,
       InitThreadRequiredMultipleWithoutProvidedKeepsMustProof) {
  const char *source = R"(
    declare i32 @MPI_Init_thread(i32*, i8***, i32, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %provided = alloca i32, align 4
      call i32 @MPI_Init_thread(i32* null, i8*** null, i32 3, i32* %provided)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getProcessModel().hasInitThreadLevel());
  EXPECT_EQ(analysis.getProcessModel().getRequiredInitThreadLevel(), 3);
  EXPECT_FALSE(analysis.getProcessModel().hasProvidedInitThreadLevel());

  bool saw_protocol_slot = false;
  bool saw_downgraded_collective = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot") {
      saw_protocol_slot = true;
      EXPECT_EQ(diag.relation.proof, concurrency::ProofStrength::Must);
    }
    if (diag.code == "mpi_collective_protocol_slot_thread_downgrade") {
      saw_downgraded_collective = true;
    }
  }
  EXPECT_TRUE(saw_protocol_slot);
  EXPECT_FALSE(saw_downgraded_collective);
}

TEST_F(MPIAnalysisTest, InitThreadProvidedMultipleDowngradesCollectiveProof) {
  const char *source = R"(
    declare i32 @MPI_Init_thread(i32*, i8***, i32, i32)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Init_thread(i32* null, i8*** null, i32 3, i32 3)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getProcessModel().hasProvidedInitThreadLevel());
  EXPECT_EQ(analysis.getProcessModel().getProvidedInitThreadLevel(), 3);

  bool saw_downgraded_collective = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot_thread_downgrade") {
      saw_downgraded_collective = true;
      EXPECT_EQ(diag.relation.proof, concurrency::ProofStrength::May);
    }
  }
  EXPECT_TRUE(saw_downgraded_collective);
}

TEST_F(MPIAnalysisTest,
       InitThreadRequiredMultipleProvidedFunneledDoesNotDowngrade) {
  const char *source = R"(
    declare i32 @MPI_Init_thread(i32*, i8***, i32, i32)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Init_thread(i32* null, i8*** null, i32 3, i32 1)
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_TRUE(analysis.getProcessModel().hasProvidedInitThreadLevel());
  EXPECT_EQ(analysis.getProcessModel().getProvidedInitThreadLevel(), 1);

  bool saw_protocol_slot = false;
  bool saw_downgraded_collective = false;
  for (const auto &diag : analysis.getResults().diagnostics) {
    if (diag.code == "mpi_collective_protocol_slot") {
      saw_protocol_slot = true;
      EXPECT_EQ(diag.relation.proof, concurrency::ProofStrength::Must);
    }
    if (diag.code == "mpi_collective_protocol_slot_thread_downgrade") {
      saw_downgraded_collective = true;
    }
  }
  EXPECT_TRUE(saw_protocol_slot);
  EXPECT_FALSE(saw_downgraded_collective);
}

TEST_F(MPIAnalysisTest, ExposesParticipantSetsChannelObligationsAndFrontiers) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %is_root = icmp eq i32 %loaded, 0
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* null)
      br i1 %is_root, label %then, label %else

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret i32 0

    else:
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  EXPECT_FALSE(analysis.getResults().participant_sets.empty());
  ASSERT_FALSE(analysis.getResults().channel_obligations.empty());
  EXPECT_EQ(analysis.getResults().channel_obligations.front().relation.kind,
            concurrency::RelationKind::MatchedCommunication);
  EXPECT_NE(analysis.getResults().channel_obligations.front()
                .sender_set.participant_class_id,
            0u);
  EXPECT_FALSE(analysis.getResults().protocol_frontiers.empty());
  EXPECT_EQ(analysis.getResults().protocol_frontiers.front().relation.kind,
            concurrency::RelationKind::SameCollectiveFrontier);
}

TEST_F(MPIAnalysisTest, NullCommunicatorPointToPointProducesStructuredModelGap) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 -2, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  ASSERT_FALSE(analysis.getResults().model_gaps.empty());
  bool saw_channel_gap = false;
  for (const auto &gap : analysis.getResults().model_gaps) {
    if (gap.domain == MPIModelGapDomain::PointToPoint) {
      saw_channel_gap = true;
    }
  }
  EXPECT_TRUE(saw_channel_gap);
}

TEST_F(MPIAnalysisTest, FlushLocalProducesLocalOnlySynchronizationFact) {
  const char *source = R"(
    @win = global i8 0

    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock_all(i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_flush_local_all(i8*)
    declare i32 @MPI_Win_unlock_all(i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock_all(i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Win_flush_local_all(i8* @win)
      call i32 @MPI_Win_unlock_all(i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_local_completion = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.completion == MPIRMACompletionStrength::Local) {
      saw_local_completion = true;
      EXPECT_EQ(fact.relation.kind,
                concurrency::RelationKind::LocalOnlySynchronizationCompletion);
    }
  }
  EXPECT_TRUE(saw_local_completion);
}

