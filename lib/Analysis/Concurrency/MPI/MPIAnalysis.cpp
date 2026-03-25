/**
 * @file MPIAnalysis.cpp
 * @brief MPI Program Analysis Coordinator Implementation
 *
 * @author Lotus Analysis Framework
 * @date 2026
 */

#include "Analysis/Concurrency/MPI/MPIAnalysis.h"

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"

#include <unordered_map>
#include <vector>

#include <llvm/IR/Instruction.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace mpi {

size_t MPIAnalysis::getProtocolDiagnosticCount(StringRef key) const {
  const auto &diagnostics = abstract_state_.protocol_diagnostics;
  auto it = diagnostics.find(key.str());
  return it != diagnostics.end() ? it->second : 0;
}

size_t MPIAnalysis::getOperationCount(MPIOpKind kind) const {
  const auto &counts = process_model_.getOperationKindCounts();
  auto it = counts.find(kind);
  return it != counts.end() ? it->second : 0;
}

size_t MPIAnalysis::getTrackedWindowCount() const {
  return abstract_state_.tracked_window_count;
}

void MPIAnalysis::runAnalysis() {
  process_model_.analyzeModule();
  collective_analysis_.analyzeCollectives();
  rma_analysis_.analyzeRMA();
  abstract_state_ =
      MPIAbstractStateBuilder(module_, process_model_, collective_analysis_,
                              rma_analysis_)
          .build();

  results_.orphaned_requests.clear();
  for (const MPIRequestFact &fact : abstract_state_.request_facts) {
    if (fact.state != MPIRequestState::Pending &&
        fact.state != MPIRequestState::Active &&
        fact.state != MPIRequestState::Created) {
      continue;
    }
    MPIProcessModel::NonBlockingOp op;
    op.issue_inst = fact.activation_inst ? fact.activation_inst : fact.origin_inst;
    op.request = fact.request;
    op.completion_state = fact.state;
    op.wait_inst = fact.last_transition_inst;
    op.peer_rank = fact.peer_rank;
    op.tag = fact.tag;
    op.comm = fact.communicator;
    results_.orphaned_requests.push_back(op);
  }
  results_.potential_deadlocks = abstract_state_.potential_deadlocks;
  results_.mismatched_collectives.clear();
  results_.conditional_collectives = abstract_state_.conditional_collective_insts;
  results_.unsynchronized_rma.clear();
  results_.rma_races.clear();
  results_.leaked_windows = abstract_state_.leaked_windows;

  const MPIOperation *first_finalize = nullptr;
  bool has_init = false;
  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.kind == MPIOpKind::FINALIZE) {
      if (first_finalize) {
        results_.double_finalize.push_back(op.inst);
      } else {
        first_finalize = &op;
      }
    }
    if (op.kind == MPIOpKind::INIT) {
      has_init = true;
    }
  }

  if (has_init && !first_finalize) {
    results_.missing_finalize = true;
  }

  results_.tag_mismatches = process_model_.findTagMismatches();
  results_.count_datatype_mismatches =
      process_model_.findCountDatatypeMismatches();
  results_.rank_out_of_bounds = process_model_.findRankOutOfBounds();
  results_.persistent_request_leaks =
      process_model_.findPersistentRequestLeaks();
  results_.wrong_root_ranks.clear();
  results_.cancel_without_wait = process_model_.findCancelWithoutWait();
  results_.buffer_overlaps = process_model_.findBufferOverlaps();
  results_.wildcard_in_collective = process_model_.findWildcardInCollective();
  results_.in_place_conflicts = process_model_.findInPlaceConflicts();
  results_.null_handles = process_model_.findNullHandles();
  results_.negative_root = process_model_.findNegativeRoot();
  results_.invalid_tags = process_model_.findInvalidTags();
  results_.invalid_ranks = process_model_.findInvalidRanks();
  results_.type_size_mismatches = process_model_.findTypeSizeMismatches();
  results_.destroy_null_comm = process_model_.findDestroyNullComm();
  results_.request_free_after_wait = process_model_.findRequestFreeAfterWait();
  results_.in_place_wrong_op = process_model_.findInPlaceWrongOp();
  results_.invalid_rma_transitions = abstract_state_.invalid_epoch_transitions;
  results_.use_after_free_windows = abstract_state_.use_after_free_windows;
  results_.double_window_free = abstract_state_.double_window_free;
  results_.process_set_facts = abstract_state_.process_set_facts;
  results_.request_set_facts = abstract_state_.request_set_facts;
  results_.participant_sets = abstract_state_.participant_sets;
  results_.channel_obligations = abstract_state_.channel_obligations;
  results_.protocol_frontiers = abstract_state_.protocol_frontiers;
  results_.rma_synchronization_facts = abstract_state_.rma_synchronization_facts;
  results_.model_gaps = abstract_state_.model_gaps;

  std::unordered_map<const Instruction *, MPICollectiveAnalysis::CollectiveCall>
      collective_call_by_inst;
  for (const auto &call : collective_analysis_.getProtocolRelations()) {
    if (call.inst) {
      collective_call_by_inst[call.inst] = call;
    }
  }
  for (const auto &pair : abstract_state_.mismatched_collective_insts) {
    auto lhs_it = collective_call_by_inst.find(pair.first);
    auto rhs_it = collective_call_by_inst.find(pair.second);
    if (lhs_it != collective_call_by_inst.end() &&
        rhs_it != collective_call_by_inst.end()) {
      results_.mismatched_collectives.emplace_back(lhs_it->second, rhs_it->second);
    }
  }
  for (const auto &pair : abstract_state_.wrong_root_inst_pairs) {
    auto lhs_it = collective_call_by_inst.find(pair.first);
    auto rhs_it = collective_call_by_inst.find(pair.second);
    if (lhs_it != collective_call_by_inst.end() &&
        rhs_it != collective_call_by_inst.end()) {
      results_.wrong_root_ranks.emplace_back(lhs_it->second, rhs_it->second);
    }
  }

  std::unordered_map<const Instruction *, MPIRMAAnalysis::RMAOperation>
      rma_relation_by_inst;
  for (const auto &relation : rma_analysis_.getSynchronizationRelations()) {
    if (relation.inst && !rma_relation_by_inst.count(relation.inst)) {
      rma_relation_by_inst[relation.inst] = relation;
    }
  }
  for (const Instruction *inst : abstract_state_.unsynchronized_rma_insts) {
    auto it = rma_relation_by_inst.find(inst);
    if (it != rma_relation_by_inst.end()) {
      results_.unsynchronized_rma.push_back(it->second);
    }
  }
  for (const auto &pair : abstract_state_.rma_race_insts) {
    auto lhs_it = rma_relation_by_inst.find(pair.first);
    auto rhs_it = rma_relation_by_inst.find(pair.second);
    if (lhs_it != rma_relation_by_inst.end() &&
        rhs_it != rma_relation_by_inst.end()) {
      results_.rma_races.emplace_back(lhs_it->second, rhs_it->second);
    }
  }

  results_.diagnostics.clear();
  for (const MPIModelGap &gap : results_.model_gaps) {
    MPIAnalysis::MPIDiagnostic diagnostic;
    diagnostic.inst = gap.inst;
    diagnostic.relation = gap.relation;
    diagnostic.model_gap_domain = gap.domain;
    diagnostic.communicator_class_id = gap.communicator_class_id;
    diagnostic.participant_class_id = gap.participant_class_id;
    diagnostic.channel_class_id = gap.channel_class_id;
    diagnostic.request_set_id = gap.request_set_id;
    diagnostic.subsystem =
        gap.provenance.empty() ? "process_model" : gap.provenance;
    if (gap.code.find("identity_unresolved") != std::string::npos) {
      diagnostic.reason_bucket = "identity_unresolved";
    } else if (gap.code.find("candidate_nonunique") != std::string::npos) {
      diagnostic.reason_bucket = "candidate_nonunique";
    } else if (gap.code.find("subset_unresolved") != std::string::npos ||
               gap.code.find("index_unresolved") != std::string::npos ||
               gap.code.find("flag_unresolved") != std::string::npos) {
      diagnostic.reason_bucket = "subset_unresolved";
    } else if (gap.code.find("summary") != std::string::npos) {
      diagnostic.reason_bucket = "summary_unresolved";
    }
    diagnostic.code = gap.code;
    diagnostic.detail = gap.detail;
    results_.diagnostics.push_back(diagnostic);
  }
  for (const MPIOperation &op : process_model_.getAllOperations()) {
    if (op.semantic_relation.kind ==
            concurrency::RelationKind::UnknownDueToModelGap &&
        op.semantic_relation.reason.empty()) {
      continue;
    }
    MPIAnalysis::MPIDiagnostic diagnostic;
    diagnostic.inst = op.inst;
    diagnostic.relation = op.semantic_relation;
    diagnostic.confidence = op.normalization_confidence;
    diagnostic.model_gap_domain =
        op.semantic_relation.kind == concurrency::RelationKind::UnknownDueToModelGap
            ? MPIModelGapDomain::Unknown
            : MPIModelGapDomain::None;
    diagnostic.communicator_class_id = op.communicator_class_id;
    diagnostic.participant_class_id = op.participant_class_id;
    diagnostic.channel_class_id = op.channel_class_id;
    diagnostic.subsystem = "process_model";
    diagnostic.code = op.semantic_relation.reason.empty()
                          ? "mpi_relation"
                          : op.semantic_relation.reason;
    diagnostic.detail = op.rank_path_summary;
    results_.diagnostics.push_back(diagnostic);
  }
}

void MPIAnalysis::printResults(raw_ostream &OS) const {
  const auto &operations = process_model_.getAllOperations();
  const auto &deferred = process_model_.getDeferredLoweringStats();

  auto countRequestStates = [&](RequestCompletionState state) {
    auto requestStatePriority = [](RequestCompletionState value) {
      switch (value) {
      case RequestCompletionState::Unbound:
        return 0;
      case RequestCompletionState::PersistentTemplate:
        return 1;
      case RequestCompletionState::InactivePersistent:
        return 2;
      case RequestCompletionState::Active:
        return 3;
      case RequestCompletionState::MayComplete:
        return 4;
      case RequestCompletionState::MustComplete:
        return 5;
      case RequestCompletionState::Canceled:
        return 6;
      case RequestCompletionState::Freed:
        return 7;
      case RequestCompletionState::Escaped:
        return 8;
      case RequestCompletionState::Unknown:
        return 9;
      }
      return 0;
    };

    std::unordered_map<RequestID, RequestCompletionState> request_states;
    for (const auto &op : operations) {
      if (op.kind != MPIOpKind::SEND_NONBLOCKING &&
          op.kind != MPIOpKind::RECV_NONBLOCKING &&
          op.kind != MPIOpKind::BARRIER_NONBLOCKING &&
          op.kind != MPIOpKind::COLLECTIVE_NONBLOCKING) {
        continue;
      }
      if (!op.request) {
        continue;
      }
      auto it = request_states.find(op.request);
      if (it == request_states.end() || requestStatePriority(op.request_state) >
                                            requestStatePriority(it->second)) {
        request_states[op.request] = op.request_state;
      }
    }

    size_t count = 0;
    for (const auto &entry : request_states) {
      if (entry.second == state ||
          (state == RequestCompletionState::Canceled &&
           entry.second == RequestCompletionState::Freed)) {
        ++count;
      }
    }
    return count;
  };

  size_t deferred_total = 0;
  for (const auto &entry : deferred) {
    if (entry.first == "unknown_flag_value" ||
        entry.first == "unknown_completed_index_set") {
      continue;
    }
    deferred_total += entry.second;
  }

  OS << "========================================\n";
  OS << "MPI Analysis Results\n";
  OS << "========================================\n\n";

  OS << "Total MPI operations found: " << operations.size() << "\n";
  OS << "MPI init/finalize ops: " << getOperationCount(MPIOpKind::INIT) << "/"
     << getOperationCount(MPIOpKind::FINALIZE) << "\n";
  OS << "MPI session ops: " << getOperationCount(MPIOpKind::SESSION) << "\n";
  OS << "Blocking point-to-point ops: "
     << getOperationCount(MPIOpKind::SEND_BLOCKING) +
            getOperationCount(MPIOpKind::RECV_BLOCKING)
     << "\n";
  OS << "Non-blocking MPI operations: "
     << getOperationCount(MPIOpKind::SEND_NONBLOCKING) +
            getOperationCount(MPIOpKind::RECV_NONBLOCKING) +
            getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "  Non-blocking point-to-point ops: "
     << getOperationCount(MPIOpKind::SEND_NONBLOCKING) +
            getOperationCount(MPIOpKind::RECV_NONBLOCKING)
     << "\n";
  OS << "  Probe ops (blocking/non-blocking): "
     << getOperationCount(MPIOpKind::PROBE_BLOCKING) << "/"
     << getOperationCount(MPIOpKind::PROBE_NONBLOCKING) << "\n";
  OS << "  Wait/Test ops: " << getOperationCount(MPIOpKind::WAIT) << "/"
     << getOperationCount(MPIOpKind::TEST) << "\n";
  OS << "Collective/barrier operations: "
     << getOperationCount(MPIOpKind::BARRIER_BLOCKING) +
            getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "  Blocking collective/barrier ops: "
     << getOperationCount(MPIOpKind::BARRIER_BLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_BLOCKING)
     << "\n";
  OS << "  Non-blocking collective/barrier ops: "
     << getOperationCount(MPIOpKind::BARRIER_NONBLOCKING) +
            getOperationCount(MPIOpKind::COLLECTIVE_NONBLOCKING)
     << "\n";
  OS << "Communicator management ops: "
     << getOperationCount(MPIOpKind::COMM_MANAGEMENT) << "\n";
  OS << "Intercommunicator creation ops: "
     << getOperationCount(MPIOpKind::INTERCOMM_CREATION) << "\n";
  OS << "Request management ops: "
     << getOperationCount(MPIOpKind::REQUEST_MANAGEMENT) << "\n";
  OS << "RMA window lifecycle ops: " << getOperationCount(MPIOpKind::RMA_WINDOW)
     << "\n";
  OS << "RMA data ops: " << getOperationCount(MPIOpKind::RMA_DATA) << "\n";
  OS << "RMA sync ops: " << getOperationCount(MPIOpKind::RMA_SYNC) << "\n";
  OS << "Collective protocol slots tracked: "
     << getProtocolDiagnosticCount("collective_slots_tracked") << "\n";
  OS << "Collective protocol frontiers tracked: "
     << results_.protocol_frontiers.size() << "\n";
  OS << "Collective partial-reachability observations: "
     << getProtocolDiagnosticCount("collective_partial_reachability") << "\n";
  OS << "Participant sets tracked: " << results_.participant_sets.size()
     << "\n";
  OS << "Request sets tracked: " << results_.request_set_facts.size() << "\n";
  OS << "Point-to-point channel obligations: "
     << results_.channel_obligations.size() << "\n";
  size_t nonunique_channel_count = 0;
  size_t unresolved_identity_channel_count = 0;
  for (const MPIChannelAutomaton &automaton : abstract_state_.channel_automata) {
    if (automaton.ambiguity_state == MPIChannelAutomaton::AmbiguityState::NonUnique) {
      ++nonunique_channel_count;
    } else if (automaton.ambiguity_state ==
               MPIChannelAutomaton::AmbiguityState::UnresolvedIdentity) {
      ++unresolved_identity_channel_count;
    }
  }
  OS << "Non-unique channel automata: " << nonunique_channel_count << "\n";
  OS << "Unresolved-identity channel automata: "
     << unresolved_identity_channel_count << "\n";
  size_t unresolved_request_sets = 0;
  for (const MPIRequestSetFact &fact : abstract_state_.request_set_facts) {
    if (fact.state == MPIRequestState::MayComplete ||
        fact.completion_scope == MPIRequestCompletionScopeKind::Unknown) {
      ++unresolved_request_sets;
    }
  }
  OS << "Unresolved request sets: " << unresolved_request_sets << "\n";
  OS << "RMA synchronization facts: "
     << results_.rma_synchronization_facts.size() << "\n";
  OS << "Structured model gaps: " << results_.model_gaps.size() << "\n";
  size_t unresolved_collective_summaries = 0;
  for (const MPIFunctionSummary &summary : abstract_state_.function_summaries) {
    if (summary.unresolved_collective_summary_effect) {
      ++unresolved_collective_summaries;
    }
  }
  OS << "Unresolved collective summaries: "
     << unresolved_collective_summaries << "\n";
  OS << "Requests with may-complete status: "
     << countRequestStates(RequestCompletionState::MayComplete) << "\n";
  OS << "Requests with terminal status: "
     << countRequestStates(RequestCompletionState::Canceled) << "\n";
  OS << "Requests with freed status: "
     << countRequestStates(RequestCompletionState::Freed) << "\n";
  const auto &normalization_counts =
      process_model_.getNormalizationConfidenceCounts();
  auto readConfidence = [&](NormalizationConfidence confidence) {
    auto it = normalization_counts.find(confidence);
    return it == normalization_counts.end() ? size_t{0} : it->second;
  };
  OS << "Normalization confidence (exact/pmpi/openmpi-forwarder/unknown): "
     << readConfidence(NormalizationConfidence::ExactMPI) << "/"
     << readConfidence(NormalizationConfidence::PMPIWrapper) << "/"
     << readConfidence(NormalizationConfidence::KnownOpenMPIForwarder) << "/"
     << readConfidence(NormalizationConfidence::UnknownVendorInternal) << "\n";
  OS << "Deferred MPI semantic lowering total: " << deferred_total << "\n\n";

  OS << "Orphaned non-blocking operations: "
     << results_.orphaned_requests.size() << "\n";
  for (const auto &req : results_.orphaned_requests) {
    OS << "  ";
    req.issue_inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  OS << "Potential deadlocks: " << results_.potential_deadlocks.size() << "\n";
  for (const auto &pair : results_.potential_deadlocks) {
    OS << "  Send: ";
    pair.first->print(OS);
    OS << "\n  Recv: ";
    pair.second->print(OS);
    OS << "\n\n";
  }

  OS << "Mismatched collectives: " << results_.mismatched_collectives.size()
     << "\n";
  for (const auto &pair : results_.mismatched_collectives) {
    OS << "  Collective 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Collective 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }

  OS << "Conditional collectives (may not be called by all processes): "
     << results_.conditional_collectives.size() << "\n";
  for (const auto *inst : results_.conditional_collectives) {
    OS << "  ";
    inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  OS << "Unsynchronized RMA operations: " << results_.unsynchronized_rma.size()
     << "\n";
  for (const auto &op : results_.unsynchronized_rma) {
    OS << "  ";
    op.inst->print(OS);
    OS << "\n";
  }
  OS << "\n";

  OS << "Potential RMA data races: " << results_.rma_races.size() << "\n";
  for (const auto &pair : results_.rma_races) {
    OS << "  Op 1: ";
    pair.first.inst->print(OS);
    OS << "\n  Op 2: ";
    pair.second.inst->print(OS);
    OS << "\n\n";
  }

  OS << "Tracked RMA windows: " << getTrackedWindowCount() << "\n";
  OS << "Leaked RMA windows: " << results_.leaked_windows.size() << "\n\n";

  if (!deferred.empty()) {
    OS << "Deferred MPI semantic lowering:\n";
    for (const auto &entry : deferred) {
      OS << "  " << entry.first << ": " << entry.second << "\n";
    }
    OS << "\n";
  }

  OS << "========================================\n";
}

} // namespace mpi
