#include "MPIAnalysisTestCommon.h"

TEST_F(MPIAnalysisTest, AbstractStateExposesCommunicatorFactsAndSummaries) {
  const char *source = R"(
    declare i32 @MPI_Comm_split(i8*, i32, i32, i8**)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @worker(i8* %comm) {
    entry:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      ret void
    }

    define i32 @main(i8* %world) {
    entry:
      %sub = alloca i8*, align 8
      call i32 @MPI_Comm_split(i8* %world, i32 0, i32 7, i8** %sub)
      %child = load i8*, i8** %sub, align 8
      call void @worker(i8* %child)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &communicators = analysis.getCommunicatorFacts();
  ASSERT_FALSE(communicators.empty());
  bool saw_split = false;
  for (const auto &fact : communicators) {
    if (fact.creation_kind == MPICommunicatorCreationKind::Split) {
      saw_split = true;
      EXPECT_NE(fact.communicator_class_id, 0u);
    }
  }
  EXPECT_TRUE(saw_split);

  const auto &summaries = analysis.getFunctionSummaries();
  ASSERT_EQ(summaries.size(), 2u);
  const auto main_summary = std::find_if(
      summaries.begin(), summaries.end(),
      [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "main";
      });
  ASSERT_NE(main_summary, summaries.end());
  EXPECT_GT(main_summary->expanded_operation_indices.size(),
            main_summary->direct_operation_indices.size());
  EXPECT_TRUE(main_summary->reaches_fixed_point);
}

TEST_F(MPIAnalysisTest, WinSyncRemainsLocalOnlyCompletion) {
  const char *source = R"(
    @win = global i8 0

    declare i32 @MPI_Win_create(i8*, i64, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i64, i32, i32, i8*)
    declare i32 @MPI_Win_sync(i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Win_create(i8* null, i64 16, i32 4, i8* %comm, i8* @win)
      call i32 @MPI_Win_lock(i32 0, i32 1, i32 0, i8* @win)
      call i32 @MPI_Put(i8* null, i32 1, i32 0, i32 1, i64 0, i32 1, i32 0,
                        i8* @win)
      call i32 @MPI_Win_sync(i8* @win)
      call i32 @MPI_Win_unlock(i32 1, i8* @win)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_local_completion = false;
  bool saw_remote_completion = false;
  for (const auto &fact : analysis.getResults().rma_synchronization_facts) {
    if (fact.completion == MPIRMACompletionStrength::Local) {
      saw_local_completion = true;
      EXPECT_EQ(fact.relation.kind,
                concurrency::RelationKind::LocalOnlySynchronizationCompletion);
    } else if (fact.completion == MPIRMACompletionStrength::Remote) {
      saw_remote_completion = true;
    }
  }
  EXPECT_TRUE(saw_local_completion);
  EXPECT_FALSE(saw_remote_completion);
}

TEST_F(MPIAnalysisTest, AbstractStateExposesRequestFactsAndChannelAutomata) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 9, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_facts = analysis.getRequestFacts();
  ASSERT_EQ(request_facts.size(), 1u);
  EXPECT_EQ(request_facts.front().kind, MPIRequestFactKind::PointToPoint);
  EXPECT_EQ(request_facts.front().state, MPIRequestState::MustComplete);
  EXPECT_EQ(request_facts.front().relation.kind,
            concurrency::RelationKind::MPIRequestCompletion);

  const auto &channel_automata = analysis.getChannelAutomata();
  ASSERT_FALSE(channel_automata.empty());
  const auto automaton = std::find_if(
      channel_automata.begin(), channel_automata.end(),
      [](const MPIChannelAutomaton &state) {
        return !state.obligations.empty() && !state.transitions.empty();
      });
  ASSERT_NE(automaton, channel_automata.end());
  EXPECT_GE(automaton->posted_receive_count, 1u);
  EXPECT_GE(automaton->transitions.size(), 2u);
}

TEST_F(MPIAnalysisTest, RequestSetFactsCaptureWaitallAndWaitanyScopes) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)
    declare i32 @MPI_Waitany(i32, i8**, i32*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      %idx = alloca i32, align 4
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 8, i8* %comm, i8* %req2)
      call i32 @MPI_Waitall(i32 2, i8** %slot0, i8* null)
      call i32 @MPI_Waitany(i32 2, i8** %slot0, i32* %idx, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_sets = analysis.getRequestSetFacts();
  bool saw_waitall = false;
  bool saw_waitany = false;
  for (const auto &fact : request_sets) {
    if (fact.provenance == "mpi_request_set_complete") {
      saw_waitall = saw_waitall ||
                    (fact.completion_scope ==
                         MPIRequestCompletionScopeKind::AllOfSet &&
                     fact.state == MPIRequestState::MustComplete &&
                     fact.requests.size() == 2);
    }
    if (fact.provenance == "mpi_request_set_may_complete") {
      saw_waitany = saw_waitany ||
                    (fact.completion_scope ==
                         MPIRequestCompletionScopeKind::OneOfSet &&
                     fact.state == MPIRequestState::MayComplete &&
                     fact.requests.size() == 2);
    }
  }
  EXPECT_TRUE(saw_waitall);
  EXPECT_TRUE(saw_waitany);
}

TEST_F(MPIAnalysisTest,
       RequestFactsAndChannelAutomataExposeRequestSetAndChannelIDs) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &request_facts = analysis.getRequestFacts();
  ASSERT_EQ(request_facts.size(), 1u);
  EXPECT_NE(request_facts.front().channel_class_id, 0u);
  EXPECT_NE(request_facts.front().request_set_id, 0u);

  const auto &request_sets = analysis.getRequestSetFacts();
  ASSERT_FALSE(request_sets.empty());
  auto request_set_it =
      std::find_if(request_sets.begin(), request_sets.end(),
                   [&](const MPIRequestSetFact &fact) {
                     return fact.request_set_id == request_facts.front().request_set_id;
                   });
  ASSERT_NE(request_set_it, request_sets.end());
  EXPECT_NE(request_set_it->channel_class_id, 0u);

  const auto &channel_automata = analysis.getChannelAutomata();
  auto automaton = std::find_if(
      channel_automata.begin(), channel_automata.end(),
      [&](const MPIChannelAutomaton &state) {
        return !state.unresolved_completion_request_set_ids.empty();
      });
  ASSERT_NE(automaton, channel_automata.end());
  EXPECT_FALSE(automaton->posted_send_obligation_ids.empty());
  EXPECT_FALSE(automaton->posted_receive_obligation_ids.empty());
  EXPECT_NE(std::find(automaton->unresolved_completion_request_set_ids.begin(),
                      automaton->unresolved_completion_request_set_ids.end(),
                      request_facts.front().request_set_id),
            automaton->unresolved_completion_request_set_ids.end());
}

TEST_F(MPIAnalysisTest, ChannelAutomatonTracksUniqueMatchTransitions) {
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

  const auto &channel_automata = analysis.getChannelAutomata();
  ASSERT_FALSE(channel_automata.empty());
  const MPIChannelAutomaton &automaton = channel_automata.front();
  EXPECT_EQ(automaton.ambiguity_state, MPIChannelAutomaton::AmbiguityState::Unique);
  EXPECT_EQ(automaton.matched_endpoint_pairs.size(), 1u);

  size_t unique_matches = 0;
  size_t post_sends = 0;
  size_t post_receives = 0;
  for (const MPIChannelTransition &transition : automaton.transitions) {
    if (transition.kind == MPIChannelTransition::Kind::UniqueMatch) {
      ++unique_matches;
      EXPECT_EQ(transition.proof, MPICommunicationMatch::MustMatch);
    } else if (transition.kind == MPIChannelTransition::Kind::PostSend) {
      ++post_sends;
    } else if (transition.kind == MPIChannelTransition::Kind::PostReceive) {
      ++post_receives;
    }
  }
  EXPECT_EQ(unique_matches, 2u);
  EXPECT_EQ(post_sends, 1u);
  EXPECT_EQ(post_receives, 1u);
}

TEST_F(MPIAnalysisTest, ChannelAutomatonTracksNonUniqueCandidates) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @main(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  bool saw_nonunique = false;
  for (const MPIChannelAutomaton &automaton : analysis.getChannelAutomata()) {
    saw_nonunique = saw_nonunique ||
                    automaton.ambiguity_state == MPIChannelAutomaton::AmbiguityState::NonUnique;
    bool has_unique_match = false;
    bool has_candidate_match = false;
    for (const MPIChannelTransition &transition : automaton.transitions) {
      has_unique_match = has_unique_match ||
                         transition.kind == MPIChannelTransition::Kind::UniqueMatch;
      has_candidate_match = has_candidate_match ||
                            transition.kind == MPIChannelTransition::Kind::CandidateMatch;
    }
    if (automaton.ambiguity_state == MPIChannelAutomaton::AmbiguityState::NonUnique) {
      EXPECT_FALSE(has_unique_match);
      EXPECT_TRUE(has_candidate_match);
    }
  }
  EXPECT_TRUE(saw_nonunique);
}

TEST_F(MPIAnalysisTest, FunctionSummariesComposeChannelAndRequestEffects) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define void @helper(i8* %comm, i8* %req) {
    entry:
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret void
    }

    define void @peer(i8* %comm) {
    entry:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call void @helper(i8* %comm, i8* %req)
      call void @peer(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  ASSERT_GE(summaries.size(), 3u);
  const auto helper_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "helper";
      });
  const auto main_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "main";
      });
  ASSERT_NE(helper_summary, summaries.end());
  ASSERT_NE(main_summary, summaries.end());

  EXPECT_FALSE(helper_summary->emitted_send_endpoint_ids.empty());
  EXPECT_FALSE(helper_summary->created_request_set_ids.empty());
  EXPECT_FALSE(helper_summary->discharged_request_set_ids.empty());
  EXPECT_FALSE(helper_summary->blocking_request_set_ids.empty());
  EXPECT_FALSE(main_summary->emitted_send_endpoint_ids.empty());
  EXPECT_FALSE(main_summary->created_request_set_ids.empty());
  EXPECT_FALSE(main_summary->discharged_request_set_ids.empty());
  EXPECT_EQ(helper_summary->emitted_send_endpoint_ids,
            main_summary->emitted_send_endpoint_ids);
  EXPECT_FALSE(helper_summary->unresolved_indirect_call_effect);
  EXPECT_FALSE(main_summary->unresolved_indirect_call_effect);
}

TEST_F(MPIAnalysisTest, FunctionSummaryMarksIndirectCallEffectsUnresolved) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)

    define void @target(i8* %comm) {
    entry:
      call i32 @MPI_Barrier(i8* %comm)
      ret void
    }

    define i32 @main(i8* %comm, void (i8*)* %fn) {
    entry:
      call void %fn(i8* %comm)
      call i32 @MPI_Barrier(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  auto main_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "main";
      });
  ASSERT_NE(main_summary, summaries.end());
  EXPECT_TRUE(main_summary->unresolved_indirect_call_effect);

  bool saw_summary_gap = false;
  for (const MPIModelGap &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_summary_indirect_call") {
      saw_summary_gap = true;
    }
  }
  EXPECT_TRUE(saw_summary_gap);
}

TEST_F(MPIAnalysisTest, FunctionSummaryTracksOutstandingChannelAndRequestState) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @worker(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 -1, i32 7, i8* %comm, i8* null)
      ret i32 0
    }

    define i32 @main(i8* %comm) {
    entry:
      %x = call i32 @worker(i8* %comm)
      ret i32 %x
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  auto worker_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "worker";
      });
  ASSERT_NE(worker_summary, summaries.end());
  EXPECT_FALSE(worker_summary->emitted_send_endpoint_ids.empty());
  EXPECT_FALSE(worker_summary->created_request_set_ids.empty());
  EXPECT_FALSE(worker_summary->outstanding_send_endpoint_ids.empty());
  EXPECT_FALSE(worker_summary->outstanding_receive_endpoint_ids.empty());
  EXPECT_FALSE(worker_summary->outstanding_request_set_ids.empty());
  EXPECT_FALSE(worker_summary->unresolved_channel_class_ids.empty());
}

TEST_F(MPIAnalysisTest, CollectiveSummaryTracksEnteredSlotsAndUnresolvedState) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)

    define void @helper(i8* %comm) {
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
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      call void @helper(i8* %comm)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  const auto &summaries = analysis.getFunctionSummaries();
  auto helper_summary = std::find_if(
      summaries.begin(), summaries.end(), [](const MPIFunctionSummary &summary) {
        return summary.function && summary.function->getName() == "helper";
      });
  ASSERT_NE(helper_summary, summaries.end());
  EXPECT_FALSE(helper_summary->collective_call_operation_indices.empty());
  EXPECT_FALSE(helper_summary->entered_collective_protocol_slots.empty());
  EXPECT_TRUE(helper_summary->unresolved_collective_summary_effect);

  bool saw_collective_gap = false;
  for (const MPIModelGap &gap : analysis.getResults().model_gaps) {
    if (gap.code == "mpi_collective_summary_unresolved" ||
        gap.code == "mpi_collective_recursive_summary_unresolved") {
      saw_collective_gap = true;
    }
  }
  EXPECT_TRUE(saw_collective_gap);
}

TEST_F(MPIAnalysisTest, ValidationKeepsRequestFactsAndChannelAutomataConsistent) {
  const char *source = R"(
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Irecv(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Waitall(i32, i8**, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %req1 = alloca i8, align 1
      %req2 = alloca i8, align 1
      %reqs = alloca [2 x i8*], align 8
      %slot0 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 0
      %slot1 = getelementptr inbounds [2 x i8*], [2 x i8*]* %reqs, i64 0, i64 1
      store i8* %req1, i8** %slot0, align 8
      store i8* %req2, i8** %slot1, align 8
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req1)
      call i32 @MPI_Irecv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req2)
      call i32 @MPI_Waitall(i32 2, i8** %slot0, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<size_t> request_set_ids;
  for (const auto &request_set : analysis.getRequestSetFacts()) {
    request_set_ids.insert(request_set.request_set_id);
  }
  std::set<size_t> channel_ids;
  for (const auto &automaton : analysis.getChannelAutomata()) {
    channel_ids.insert(automaton.channel_class_id);
  }

  for (const MPIRequestFact &fact : analysis.getRequestFacts()) {
    if (fact.request_set_id != 0) {
      EXPECT_NE(request_set_ids.count(fact.request_set_id), 0u);
    }
    if (fact.channel_class_id != 0) {
      EXPECT_NE(channel_ids.count(fact.channel_class_id), 0u);
    }
  }
}

TEST_F(MPIAnalysisTest, ValidationKeepsChannelObligationsAndAutomataConsistent) {
  const char *source = R"(
    declare i32 @MPI_Send(i8*, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Recv(i8*, i32, i32, i32, i32, i8*, i8*)

    define i32 @rank0(i8* %comm) {
    entry:
      call i32 @MPI_Send(i8* null, i32 1, i32 0, i32 1, i32 7, i8* %comm)
      ret i32 0
    }

    define i32 @rank1(i8* %comm) {
    entry:
      call i32 @MPI_Recv(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* null)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<std::pair<size_t, size_t>> matched_pairs;
  for (const MPIChannelObligation &obligation : analysis.getResults().channel_obligations) {
    EXPECT_NE(obligation.sender_obligation_id, 0u);
    EXPECT_NE(obligation.receiver_obligation_id, 0u);
    matched_pairs.emplace(obligation.sender_obligation_id, obligation.receiver_obligation_id);
  }

  for (const MPIChannelAutomaton &automaton : analysis.getChannelAutomata()) {
    for (const auto &pair : automaton.matched_endpoint_pairs) {
      EXPECT_NE(matched_pairs.count(pair), 0u);
    }
  }
}

TEST_F(MPIAnalysisTest, ValidationKeepsSummaryReferencesConsistent) {
  const char *source = R"(
    declare i32 @MPI_Comm_rank(i8*, i32*)
    declare i32 @MPI_Bcast(i8*, i32, i32, i32, i8*)
    declare i32 @MPI_Isend(i8*, i32, i32, i32, i32, i8*, i8*)
    declare i32 @MPI_Wait(i8*, i8*)

    define void @helper(i8* %comm, i8* %req) {
    entry:
      %rank = alloca i32, align 4
      call i32 @MPI_Comm_rank(i8* %comm, i32* %rank)
      %loaded = load i32, i32* %rank, align 4
      %root = icmp eq i32 %loaded, 0
      br i1 %root, label %then, label %join

    then:
      call i32 @MPI_Bcast(i8* null, i32 1, i32 0, i32 0, i8* %comm)
      br label %join

    join:
      call i32 @MPI_Isend(i8* null, i32 1, i32 0, i32 0, i32 7, i8* %comm, i8* %req)
      call i32 @MPI_Wait(i8* %req, i8* null)
      ret void
    }

    define i32 @main(i8* %comm) {
    entry:
      %req = alloca i8, align 1
      call void @helper(i8* %comm, i8* %req)
      ret i32 0
    }
  )";

  auto module = parseModule(source);
  ASSERT_NE(module, nullptr);

  MPIAnalysis analysis(*module);
  analysis.runAnalysis();

  std::set<size_t> request_set_ids;
  for (const auto &request_set : analysis.getRequestSetFacts()) {
    request_set_ids.insert(request_set.request_set_id);
  }
  std::set<size_t> frontier_ids;
  for (const auto &frontier : analysis.getResults().protocol_frontiers) {
    frontier_ids.insert(frontier.frontier_id);
  }

  for (const MPIFunctionSummary &summary : analysis.getFunctionSummaries()) {
    for (size_t id : summary.outstanding_request_set_ids) {
      EXPECT_NE(request_set_ids.count(id), 0u);
    }
    for (size_t id : summary.outstanding_collective_frontier_ids) {
      EXPECT_NE(frontier_ids.count(id), 0u);
    }
  }
}

TEST_F(MPIAnalysisTest, PrintResultsIncludesValidationCounters) {
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

  std::string output;
  raw_string_ostream os(output);
  analysis.printResults(os);
  os.flush();

  EXPECT_NE(output.find("Request sets tracked:"), std::string::npos);
  EXPECT_NE(output.find("Non-unique channel automata:"), std::string::npos);
  EXPECT_NE(output.find("Unresolved-identity channel automata:"),
            std::string::npos);
  EXPECT_NE(output.find("Unresolved request sets:"), std::string::npos);
  EXPECT_NE(output.find("Unresolved collective summaries:"),
            std::string::npos);
}

TEST_F(MPIAnalysisTest, AbstractStateExposesProtocolAndEpochFacts) {
  const char *source = R"(
    declare i32 @MPI_Barrier(i8*)
    declare i32 @MPI_Win_create(i8*, i32, i32, i8*, i8*)
    declare i32 @MPI_Win_lock(i32, i32, i32, i8*)
    declare i32 @MPI_Put(i8*, i32, i32, i32, i32, i32, i32, i8*)
    declare i32 @MPI_Win_flush(i32, i8*)
    declare i32 @MPI_Win_unlock(i32, i8*)

    define i32 @main(i8* %comm) {
    entry:
      %win = alloca i8, align 1
      call i32 @MPI_Barrier(i8* %comm)
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

  const auto &protocol_states = analysis.getCollectiveProtocolStates();
  ASSERT_FALSE(protocol_states.empty());
  EXPECT_EQ(protocol_states.front().relation.kind,
            concurrency::RelationKind::MPICollectiveParticipation);
  EXPECT_FALSE(protocol_states.front().operations.empty());

  const auto &epoch_facts = analysis.getRMAEpochFacts();
  ASSERT_FALSE(epoch_facts.empty());
  bool saw_epoch = false;
  for (const auto &fact : epoch_facts) {
    if (fact.epoch_id != 0 && !fact.operations.empty()) {
      saw_epoch = true;
    }
  }
  EXPECT_TRUE(saw_epoch);
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
