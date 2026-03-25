#include "Analysis/Concurrency/MPI/MPIAbstractState.h"

#include "Analysis/Concurrency/MPI/MPICollectiveAnalysis.h"
#include "Analysis/Concurrency/MPI/MPIProcessModel.h"
#include "Analysis/Concurrency/MPI/MPIRMAAnalysis.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace mpi {

void MPIAbstractState::clear() {
  communicator_fact_by_class.clear();
  request_fact_by_request.clear();
  channel_automata_by_key.clear();
  collective_state_by_scope.clear();
  rma_epoch_by_key.clear();
  protocol_diagnostics.clear();

  communicator_facts.clear();
  function_summaries.clear();
  request_facts.clear();
  request_set_facts.clear();
  channel_automata.clear();
  collective_protocol_states.clear();
  rma_epoch_facts.clear();
  process_set_facts.clear();
  participant_sets.clear();
  channel_obligations.clear();
  protocol_frontiers.clear();
  rma_synchronization_facts.clear();
  model_gaps.clear();
  potential_deadlocks.clear();
  mismatched_collective_insts.clear();
  conditional_collective_insts.clear();
  wrong_root_inst_pairs.clear();
  unsynchronized_rma_insts.clear();
  rma_race_insts.clear();
  leaked_windows.clear();
  invalid_epoch_transitions.clear();
  use_after_free_windows.clear();
  double_window_free.clear();
  tracked_window_count = 0;
}

MPICommunicatorFact &
MPIAbstractState::upsertCommunicatorFact(size_t communicator_class_id) {
  return communicator_fact_by_class[communicator_class_id];
}

MPIRequestFact &MPIAbstractState::upsertRequestFact(RequestID request) {
  return request_fact_by_request[request];
}

MPIChannelAutomaton &
MPIAbstractState::upsertChannelAutomaton(const std::string &key) {
  return channel_automata_by_key[key];
}

MPICollectiveProtocolState &MPIAbstractState::upsertCollectiveState(
    size_t communicator_class_id, size_t communicator_subgroup_id,
    size_t participant_class_id, size_t protocol_class_id,
    size_t protocol_position) {
  return collective_state_by_scope[std::make_tuple(
      communicator_class_id, communicator_subgroup_id, participant_class_id,
      protocol_class_id, protocol_position)];
}

MPIRMAEpochFact &MPIAbstractState::upsertRMAEpochFact(
    size_t participant_class_id, WindowID window, size_t epoch_id) {
  return rma_epoch_by_key[std::make_tuple(participant_class_id, window, epoch_id)];
}

namespace {

bool isSendOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::SEND_BLOCKING || kind == MPIOpKind::SEND_NONBLOCKING;
}

bool isRecvOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::RECV_BLOCKING || kind == MPIOpKind::RECV_NONBLOCKING;
}

bool isCollectiveOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::BARRIER_BLOCKING ||
         kind == MPIOpKind::BARRIER_NONBLOCKING ||
         kind == MPIOpKind::COLLECTIVE_BLOCKING ||
         kind == MPIOpKind::COLLECTIVE_NONBLOCKING;
}

bool isRMAOperationKind(MPIOpKind kind) {
  return kind == MPIOpKind::RMA_WINDOW || kind == MPIOpKind::RMA_DATA ||
         kind == MPIOpKind::RMA_SYNC;
}

bool rangesOverlap(int lhs_min, int lhs_max, int rhs_min, int rhs_max) {
  if (lhs_min < 0 || lhs_max < 0 || rhs_min < 0 || rhs_max < 0) {
    return true;
  }
  return !(lhs_max < rhs_min || rhs_max < lhs_min);
}

std::string classifyTag(int tag) {
  if (tag < 0) {
    return "wildcard";
  }
  return std::to_string(tag);
}

int64_t classifyDatatypeCountSize(const MPIOperation &op) {
  if (op.byte_length > 0) {
    return op.byte_length;
  }
  return op.datatype_size;
}

std::string buildChannelKey(const MPIOperation &send, const MPIOperation &recv) {
  return std::to_string(send.communicator_class_id != 0
                            ? send.communicator_class_id
                            : recv.communicator_class_id) +
         ":" + send.participant_set.toKey() + ":" + recv.participant_set.toKey() +
         ":" + classifyTag(send.tag >= 0 ? send.tag : recv.tag) + ":" +
         std::to_string(static_cast<int>(send.send_mode)) + ":" +
         std::to_string(std::max(classifyDatatypeCountSize(send),
                                 classifyDatatypeCountSize(recv)));
}

bool communicatorsCompatible(const MPICollectiveAnalysis::CollectiveCall &lhs,
                             const MPICollectiveAnalysis::CollectiveCall &rhs) {
  if (lhs.communicator_class_id != 0 && rhs.communicator_class_id != 0) {
    return lhs.communicator_class_id == rhs.communicator_class_id;
  }
  return lhs.comm && rhs.comm && lhs.comm == rhs.comm;
}

bool areCollectivesCompatible(const MPICollectiveAnalysis::CollectiveCall &lhs,
                              const MPICollectiveAnalysis::CollectiveCall &rhs) {
  if (!communicatorsCompatible(lhs, rhs)) {
    return true;
  }
  if (lhs.type != rhs.type) {
    return false;
  }
  if (lhs.blocking_mode != MPIBlockingMode::Unknown &&
      rhs.blocking_mode != MPIBlockingMode::Unknown &&
      lhs.blocking_mode != rhs.blocking_mode) {
    return false;
  }
  if (lhs.collective_variant != MPICollectiveVariant::Unknown &&
      rhs.collective_variant != MPICollectiveVariant::Unknown &&
      lhs.collective_variant != rhs.collective_variant) {
    return false;
  }
  if (lhs.collective_shape != MPICollectiveShape::Unknown &&
      rhs.collective_shape != MPICollectiveShape::Unknown &&
      lhs.collective_shape != rhs.collective_shape) {
    return false;
  }
  if (lhs.root_rank >= 0 && rhs.root_rank >= 0 && lhs.root_rank != rhs.root_rank) {
    return false;
  }
  if (lhs.count >= 0 && rhs.count >= 0 && lhs.count != rhs.count) {
    return false;
  }
  if (lhs.recv_count >= 0 && rhs.recv_count >= 0 &&
      lhs.recv_count != rhs.recv_count) {
    return false;
  }
  if (lhs.datatype >= 0 && rhs.datatype >= 0 && lhs.datatype != rhs.datatype) {
    return false;
  }
  if (lhs.recv_datatype >= 0 && rhs.recv_datatype >= 0 &&
      lhs.recv_datatype != rhs.recv_datatype) {
    return false;
  }
  if (lhs.reduction_op >= 0 && rhs.reduction_op >= 0 &&
      lhs.reduction_op != rhs.reduction_op) {
    return false;
  }
  return lhs.in_place == rhs.in_place;
}

bool participantsMayOverlap(const MPIParticipantSet &lhs,
                            const MPIParticipantSet &rhs) {
  return lhs.unknown || rhs.unknown || lhs.mayOverlap(rhs);
}

bool isRMAWriteAccess(MPIRMAAccessKind kind) {
  return kind == MPIRMAAccessKind::Put || kind == MPIRMAAccessKind::Accumulate ||
         kind == MPIRMAAccessKind::Atomic;
}

void emitValidationGap(MPIAbstractState &state, MPIModelGapDomain domain,
                       const Instruction *inst, llvm::StringRef code,
                       llvm::StringRef provenance, llvm::StringRef detail,
                       size_t communicator_class_id = 0,
                       size_t participant_class_id = 0,
                       size_t channel_class_id = 0,
                       size_t request_set_id = 0) {
  MPIModelGap gap;
  gap.domain = domain;
  gap.inst = inst;
  gap.communicator_class_id = communicator_class_id;
  gap.participant_class_id = participant_class_id;
  gap.channel_class_id = channel_class_id;
  gap.request_set_id = request_set_id;
  gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
  gap.relation.proof = concurrency::ProofStrength::Unknown;
  gap.relation.reason = code.str();
  gap.code = code.str();
  gap.provenance = provenance.str();
  gap.detail = detail.str();
  state.model_gaps.push_back(gap);
}

bool requestSetExists(const std::vector<MPIRequestSetFact> &request_sets,
                      size_t request_set_id) {
  return request_set_id == 0 ||
         std::any_of(request_sets.begin(), request_sets.end(),
                     [&](const MPIRequestSetFact &fact) {
                       return fact.request_set_id == request_set_id;
                     });
}

bool channelClassExists(const std::vector<MPIChannelAutomaton> &automata,
                        size_t channel_class_id) {
  return channel_class_id == 0 ||
         std::any_of(automata.begin(), automata.end(),
                     [&](const MPIChannelAutomaton &automaton) {
                       return automaton.channel_class_id == channel_class_id;
                     });
}

} // namespace

MPIAbstractState MPIAbstractStateBuilder::build() const {
  MPIAbstractState state;
  // Builder-owned semantic state starts from normalized emitter facts, then
  // executes channel/request/collective projections and validation before any
  // public results are exposed.
  state.process_set_facts = process_model_.getProcessSetFacts();
  state.participant_sets = process_model_.getParticipantSets();
  state.model_gaps = process_model_.getModelGaps();
  const auto &rma_model_gaps = rma_analysis_.getModelGaps();
  state.model_gaps.insert(state.model_gaps.end(), rma_model_gaps.begin(),
                          rma_model_gaps.end());
  state.function_summaries = process_model_.getFunctionSummaries();
  state.request_set_facts = process_model_.getRequestSetFacts();
  std::unordered_map<const Function *, size_t> function_summary_index;
  for (size_t idx = 0; idx < state.function_summaries.size(); ++idx) {
    if (state.function_summaries[idx].function) {
      function_summary_index[state.function_summaries[idx].function] = idx;
    }
  }

  for (const MPICommunicatorFact &fact : process_model_.getCommunicatorFacts()) {
    MPICommunicatorFact &slot = state.upsertCommunicatorFact(fact.communicator_class_id);
    slot = fact;
  }

  const auto &operations = process_model_.getAllOperations();
  const auto &events = process_model_.getSemanticEvents();
  const auto &request_summaries = process_model_.getRequestStateSummaries();
  const auto &request_sets = process_model_.getRequestSetFacts();
  const auto &channel_endpoints = process_model_.getChannelEndpointObligations();
  const auto &source_channel_obligations = process_model_.getChannelObligations();
  const auto &protocol_relations = collective_analysis_.getProtocolRelations();
  const auto &rma_relations = rma_analysis_.getSynchronizationRelations();
  const auto &rma_facts = rma_analysis_.getSynchronizationFacts();
  const MPI::MPIRankAnalysis *rank_analysis = process_model_.getRankAnalysis();

  std::unordered_map<const Instruction *, size_t> op_index_by_inst;
  std::unordered_map<const Instruction *, MPICollectiveAnalysis::CollectiveCall>
      collective_call_by_inst;
  std::unordered_map<const Instruction *, MPIRMAAnalysis::RMAOperation>
      rma_relation_by_inst;
  std::unordered_map<const Instruction *, RMASynchronizationFact> rma_fact_by_inst;

  for (size_t idx = 0; idx < operations.size(); ++idx) {
    if (operations[idx].inst) {
      op_index_by_inst[operations[idx].inst] = idx;
    }
  }
  for (const auto &call : protocol_relations) {
    if (call.inst) {
      collective_call_by_inst[call.inst] = call;
    }
  }
  for (const auto &relation : rma_relations) {
    if (relation.inst && !rma_relation_by_inst.count(relation.inst)) {
      rma_relation_by_inst[relation.inst] = relation;
    }
  }
  for (const auto &fact : rma_facts) {
    if (fact.inst && !rma_fact_by_inst.count(fact.inst)) {
      rma_fact_by_inst[fact.inst] = fact;
    }
  }

  std::unordered_map<const Instruction *, std::vector<size_t>> operations_by_instruction;
  for (size_t idx = 0; idx < operations.size(); ++idx) {
    if (operations[idx].inst) {
      operations_by_instruction[operations[idx].inst].push_back(idx);
    }
  }

  state.channel_obligations.clear();
  std::unordered_map<size_t, const MPIChannelEndpointObligation *> endpoint_by_id;
  for (const MPIChannelEndpointObligation &endpoint : channel_endpoints) {
    endpoint_by_id[endpoint.obligation_id] = &endpoint;
  }

  for (const MPIChannelEndpointObligation &endpoint : channel_endpoints) {
    const MPIOperation &op = operations[endpoint.operation_index];
    std::string key = std::to_string(endpoint.channel_class_id) + ":" +
                      (endpoint.endpoint_kind == MPIChannelEndpointKind::Send
                           ? endpoint.participants.toKey()
                           : "") +
                      ":" +
                      (endpoint.endpoint_kind == MPIChannelEndpointKind::Receive
                           ? endpoint.participants.toKey()
                           : "");
    MPIChannelAutomaton &automaton =
        state.upsertChannelAutomaton(std::to_string(endpoint.channel_class_id) + ":" +
                                     std::to_string(endpoint.communicator_class_id));
    automaton.channel_class_id = endpoint.channel_class_id;
    automaton.communicator_class_id = endpoint.communicator_class_id;
    if (endpoint.endpoint_kind == MPIChannelEndpointKind::Send) {
      automaton.sender_set = endpoint.participants;
      automaton.posted_send_obligation_ids.push_back(endpoint.obligation_id);
      if (endpoint.blocking) {
        ++automaton.inflight_send_count;
      }
    } else {
      automaton.receiver_set = endpoint.participants;
      automaton.posted_receive_obligation_ids.push_back(endpoint.obligation_id);
      if (endpoint.blocking) {
        ++automaton.posted_receive_count;
      }
    }
    automaton.tag = endpoint.tag;
    automaton.tag_class = endpoint.tag_class;
    automaton.send_mode = endpoint.send_mode;
    automaton.datatype_size_class = endpoint.datatype_size;
    automaton.has_wildcard_endpoint =
        automaton.has_wildcard_endpoint || endpoint.tag_class == MPITagClassKind::Wildcard ||
        endpoint.participants.unknown;
    if (!endpoint.communicator_resolved) {
      automaton.ambiguity_state =
          MPIChannelAutomaton::AmbiguityState::UnresolvedIdentity;
    } else if (automaton.ambiguity_state !=
               MPIChannelAutomaton::AmbiguityState::UnresolvedIdentity &&
               endpoint.candidate_ids.size() > 1) {
      automaton.ambiguity_state = MPIChannelAutomaton::AmbiguityState::NonUnique;
    }

    MPIChannelTransition transition;
    transition.operation_index = endpoint.operation_index;
    transition.endpoint_obligation_id = endpoint.obligation_id;
    transition.inst = endpoint.inst;
    transition.is_send = endpoint.endpoint_kind == MPIChannelEndpointKind::Send;
    transition.is_recv = endpoint.endpoint_kind == MPIChannelEndpointKind::Receive;
    transition.blocking = endpoint.blocking;
    transition.kind = endpoint.endpoint_kind == MPIChannelEndpointKind::Send
                          ? MPIChannelTransition::Kind::PostSend
                          : MPIChannelTransition::Kind::PostReceive;
    transition.proof = endpoint.communicator_resolved ? MPICommunicationMatch::MayMatch
                                                      : MPICommunicationMatch::Unknown;
    transition.proof_source = endpoint.communicator_resolved
                                  ? "posted_endpoint"
                                  : "mpi_channel_identity_unresolved";
    transition.relation = op.semantic_relation;
    automaton.transitions.push_back(transition);
  }

  for (const MPIChannelObligation &channel : source_channel_obligations) {
    if (channel.sender_operation_index >= operations.size() ||
        channel.receiver_operation_index >= operations.size()) {
      continue;
    }
    const MPIOperation &send = operations[channel.sender_operation_index];
    const MPIOperation &recv = operations[channel.receiver_operation_index];

    MPIChannelAutomaton &automaton =
        state.upsertChannelAutomaton(std::to_string(channel.channel_class_id) + ":" +
                                     std::to_string(channel.communicator_class_id));
    automaton.channel_class_id = channel.channel_class_id;
    automaton.communicator_class_id = channel.communicator_class_id;
    automaton.sender_set = channel.sender_set;
    automaton.receiver_set = channel.receiver_set;
    automaton.tag = channel.tag;
    automaton.tag_class = channel.tag_class;
    automaton.send_mode = channel.send_mode;
    automaton.datatype_size_class =
        std::max(classifyDatatypeCountSize(send), classifyDatatypeCountSize(recv));
    automaton.has_wildcard_endpoint =
        channel.tag_class == MPITagClassKind::Wildcard ||
        channel.sender_set.unknown || channel.receiver_set.unknown;
    automaton.matched_endpoint_pairs.emplace_back(channel.sender_obligation_id,
                                                  channel.receiver_obligation_id);
    if (!channel.discharged) {
      ++automaton.unresolved_obligation_count;
      automaton.unresolved_send_obligation_ids.push_back(channel.sender_obligation_id);
      automaton.unresolved_receive_obligation_ids.push_back(channel.receiver_obligation_id);
      if (channel.request_set_id != 0) {
        automaton.unresolved_completion_request_set_ids.push_back(
            channel.request_set_id);
      }
    }
    if (channel.proof == MPICommunicationMatch::MayMatch &&
        automaton.ambiguity_state !=
            MPIChannelAutomaton::AmbiguityState::UnresolvedIdentity) {
      automaton.ambiguity_state = MPIChannelAutomaton::AmbiguityState::NonUnique;
    }
    if (channel.proof == MPICommunicationMatch::Unknown) {
      automaton.ambiguity_state =
          MPIChannelAutomaton::AmbiguityState::UnresolvedIdentity;
    }
    automaton.obligations.push_back(channel);

    MPIChannelTransition send_transition;
    send_transition.operation_index = channel.sender_operation_index;
    send_transition.endpoint_obligation_id = channel.sender_obligation_id;
    send_transition.request_set_id = channel.request_set_id;
    send_transition.inst = channel.sender_inst;
    send_transition.is_send = true;
    send_transition.blocking = channel.send_is_blocking;
    send_transition.discharged = channel.discharged;
    send_transition.kind = channel.proof == MPICommunicationMatch::MustMatch
                               ? MPIChannelTransition::Kind::UniqueMatch
                               : MPIChannelTransition::Kind::CandidateMatch;
    send_transition.proof = channel.proof;
    send_transition.proof_source = channel.proof_source;
    send_transition.relation = channel.relation;
    automaton.transitions.push_back(send_transition);

    MPIChannelTransition recv_transition;
    recv_transition.operation_index = channel.receiver_operation_index;
    recv_transition.endpoint_obligation_id = channel.receiver_obligation_id;
    recv_transition.request_set_id = channel.request_set_id;
    recv_transition.inst = channel.receiver_inst;
    recv_transition.is_recv = true;
    recv_transition.blocking = channel.recv_is_blocking;
    recv_transition.discharged = channel.discharged;
    recv_transition.kind = channel.proof == MPICommunicationMatch::MustMatch
                               ? MPIChannelTransition::Kind::UniqueMatch
                               : MPIChannelTransition::Kind::CandidateMatch;
    recv_transition.proof = channel.proof;
    recv_transition.proof_source = channel.proof_source;
    recv_transition.relation = channel.relation;
    automaton.transitions.push_back(recv_transition);

    if (channel.discharged && channel.request_set_id != 0) {
      MPIChannelTransition discharge_transition;
      discharge_transition.operation_index = channel.sender_operation_index;
      discharge_transition.endpoint_obligation_id = channel.sender_obligation_id;
      discharge_transition.request_set_id = channel.request_set_id;
      discharge_transition.inst = channel.discharge_inst;
      discharge_transition.discharged = true;
      discharge_transition.proof = channel.proof;
      discharge_transition.proof_source = channel.proof_source;
      if (channel.discharge_inst) {
        auto discharge_op_it = op_index_by_inst.find(channel.discharge_inst);
        if (discharge_op_it != op_index_by_inst.end()) {
          const MPIOperation &discharge_op = operations[discharge_op_it->second];
          if (discharge_op.kind == MPIOpKind::WAIT) {
            discharge_transition.kind = MPIChannelTransition::Kind::DischargeByWait;
          } else if (discharge_op.kind == MPIOpKind::TEST) {
            discharge_transition.kind = MPIChannelTransition::Kind::DischargeByTest;
          } else if (discharge_op.td_type == ThreadAPI::TD_MPI_CANCEL ||
                     discharge_op.td_type == ThreadAPI::TD_MPI_REQUEST_FREE) {
            discharge_transition.kind =
                MPIChannelTransition::Kind::ReleaseByCancelOrFree;
          } else {
            discharge_transition.kind = MPIChannelTransition::Kind::Unknown;
          }
        }
      }
      automaton.transitions.push_back(discharge_transition);
    }
  }

  for (const auto &entry : state.channel_automata_by_key) {
    MPIChannelAutomaton automaton = entry.second;
    std::sort(automaton.posted_send_obligation_ids.begin(),
              automaton.posted_send_obligation_ids.end());
    automaton.posted_send_obligation_ids.erase(
        std::unique(automaton.posted_send_obligation_ids.begin(),
                    automaton.posted_send_obligation_ids.end()),
        automaton.posted_send_obligation_ids.end());
    std::sort(automaton.posted_receive_obligation_ids.begin(),
              automaton.posted_receive_obligation_ids.end());
    automaton.posted_receive_obligation_ids.erase(
        std::unique(automaton.posted_receive_obligation_ids.begin(),
                    automaton.posted_receive_obligation_ids.end()),
        automaton.posted_receive_obligation_ids.end());
    std::sort(automaton.unresolved_send_obligation_ids.begin(),
              automaton.unresolved_send_obligation_ids.end());
    automaton.unresolved_send_obligation_ids.erase(
        std::unique(automaton.unresolved_send_obligation_ids.begin(),
                    automaton.unresolved_send_obligation_ids.end()),
        automaton.unresolved_send_obligation_ids.end());
    std::sort(automaton.unresolved_receive_obligation_ids.begin(),
              automaton.unresolved_receive_obligation_ids.end());
    automaton.unresolved_receive_obligation_ids.erase(
        std::unique(automaton.unresolved_receive_obligation_ids.begin(),
                    automaton.unresolved_receive_obligation_ids.end()),
        automaton.unresolved_receive_obligation_ids.end());
    std::sort(automaton.unresolved_completion_request_set_ids.begin(),
              automaton.unresolved_completion_request_set_ids.end());
    automaton.unresolved_completion_request_set_ids.erase(
        std::unique(automaton.unresolved_completion_request_set_ids.begin(),
                    automaton.unresolved_completion_request_set_ids.end()),
        automaton.unresolved_completion_request_set_ids.end());
    automaton.inflight_send_count = automaton.posted_send_obligation_ids.size();
    automaton.posted_receive_count = automaton.posted_receive_obligation_ids.size();
    automaton.unresolved_obligation_count =
        automaton.unresolved_send_obligation_ids.size() +
        automaton.unresolved_receive_obligation_ids.size();
    std::stable_sort(automaton.transitions.begin(), automaton.transitions.end(),
                     [&](const MPIChannelTransition &lhs,
                         const MPIChannelTransition &rhs) {
                       return lhs.operation_index < rhs.operation_index;
                     });
    for (const MPIChannelObligation &channel : automaton.obligations) {
      state.channel_obligations.push_back(channel);
    }
    state.channel_automata.push_back(std::move(automaton));
  }

  {
    std::set<size_t> unresolved_request_set_ids;
    for (const MPIChannelAutomaton &automaton : state.channel_automata) {
      unresolved_request_set_ids.insert(
          automaton.unresolved_completion_request_set_ids.begin(),
          automaton.unresolved_completion_request_set_ids.end());
    }

    std::unordered_map<RequestID, const MPIRequestSetFact *> latest_request_set_by_request;
    for (const MPIRequestSetFact &request_set : request_sets) {
      for (RequestID request : request_set.requests) {
        if (!request) {
          continue;
        }
        auto it = latest_request_set_by_request.find(request);
        if (it == latest_request_set_by_request.end() ||
            it->second->request_set_id < request_set.request_set_id) {
          latest_request_set_by_request[request] = &request_set;
        }
      }
    }

    state.request_fact_by_request.clear();
    state.request_facts.clear();
    for (const auto &entry : request_summaries) {
      const MPIRequestStateSummary &summary = entry.second;
      MPIRequestFact &fact = state.upsertRequestFact(summary.request);
      fact.request = summary.request;
      fact.kind = summary.is_persistent
                      ? MPIRequestFactKind::Persistent
                      : (summary.is_collective ? MPIRequestFactKind::Collective
                                               : MPIRequestFactKind::PointToPoint);
      fact.origin_inst = summary.origin_inst;
      fact.activation_inst = summary.activation_inst;
      fact.last_transition_inst = summary.last_transition_inst;
      fact.is_persistent = summary.is_persistent;
      fact.is_collective = summary.is_collective;
      fact.peer_rank = summary.peer_rank;
      fact.tag = summary.tag;
      fact.communicator = summary.communicator;
      fact.communicator_class_id =
          process_model_.getCommunicatorClassID(summary.communicator);
      fact.send_mode = summary.send_mode;

      const MPIRequestSetFact *latest_request_set = nullptr;
      if (summary.request_set_id != 0) {
        for (const MPIRequestSetFact &request_set : request_sets) {
          if (request_set.request_set_id == summary.request_set_id) {
            latest_request_set = &request_set;
            break;
          }
        }
      }
      if (!latest_request_set) {
        auto latest_it = latest_request_set_by_request.find(summary.request);
        if (latest_it != latest_request_set_by_request.end()) {
          latest_request_set = latest_it->second;
        }
      }

      fact.request_set_id =
          latest_request_set ? latest_request_set->request_set_id : summary.request_set_id;
      fact.channel_class_id =
          latest_request_set && latest_request_set->channel_class_id != 0
              ? latest_request_set->channel_class_id
              : summary.channel_class_id;
      fact.completion_scope =
          latest_request_set ? latest_request_set->completion_scope
                             : summary.completion_scope;
      fact.provenance =
          latest_request_set ? latest_request_set->provenance : summary.provenance;

      MPIRequestState derived_state =
          latest_request_set ? latest_request_set->state : summary.state;
      if (derived_state == MPIRequestState::MustComplete &&
          unresolved_request_set_ids.count(fact.request_set_id) != 0) {
        derived_state = MPIRequestState::MayComplete;
      }
      fact.state = derived_state;
      fact.relation.kind = fact.state == MPIRequestState::MustComplete
                               ? concurrency::RelationKind::MPIRequestCompletion
                               : concurrency::RelationKind::UnknownDueToModelGap;
      fact.relation.proof = fact.state == MPIRequestState::MustComplete
                                ? concurrency::ProofStrength::Must
                                : (fact.state == MPIRequestState::MayComplete
                                       ? concurrency::ProofStrength::May
                                       : concurrency::ProofStrength::Unknown);
      fact.relation.reason = fact.state == MPIRequestState::MustComplete
                                 ? "mpi_request_fact_complete"
                                 : "mpi_request_fact_state";
    }

    for (const auto &entry : state.request_fact_by_request) {
      state.request_facts.push_back(entry.second);
    }
  }

  std::unordered_map<size_t, size_t> collective_position_by_op;
  for (const MPIFunctionSummary &summary : state.function_summaries) {
    size_t collective_position = 0;
    for (size_t op_index : summary.expanded_collective_operation_indices) {
      if (op_index >= operations.size() || !isCollectiveOperationKind(operations[op_index].kind)) {
        continue;
      }
      collective_position_by_op.emplace(op_index, collective_position++);
    }
  }

  std::set<const Instruction *> seen_conditional_collectives;
  for (const MPIEvent &event : events) {
    if (!event.has_collective_semantics) {
      continue;
    }

    const MPIOperation &op = operations[event.operation_index];
    size_t protocol_position = collective_position_by_op.count(event.operation_index)
                                   ? collective_position_by_op[event.operation_index]
                                   : 0;
    MPICollectiveProtocolState &protocol_state = state.upsertCollectiveState(
        op.communicator_class_id, op.communicator_subgroup_id,
        op.participant_class_id, op.collective_protocol_class_id,
        protocol_position);
    protocol_state.communicator_class_id = op.communicator_class_id;
    protocol_state.communicator_subgroup_id = op.communicator_subgroup_id;
    protocol_state.participant_class_id = op.participant_class_id;
    protocol_state.protocol_class_id = op.collective_protocol_class_id;
    protocol_state.protocol_position = protocol_position;
    protocol_state.type = op.td_type;
    protocol_state.variant = op.collective_variant;
    protocol_state.shape = op.collective_shape;
    protocol_state.reachability = event.collective.reachability;
    protocol_state.operations.push_back(op.inst);
    protocol_state.relation.kind =
        concurrency::RelationKind::MPICollectiveParticipation;
    protocol_state.relation.proof =
        event.collective.reachability == ProtocolReachability::AllRanks
            ? concurrency::ProofStrength::Must
            : concurrency::ProofStrength::May;
    protocol_state.relation.reason = "mpi_collective_summary_position";
    state.protocol_diagnostics["collective_slots_tracked"]++;
    if (event.collective.reachability != ProtocolReachability::AllRanks) {
      state.protocol_diagnostics["collective_partial_reachability"]++;
    }

    bool conditional = event.collective.reachability == ProtocolReachability::SomeRanks;
    if (!conditional && event.collective.reachability == ProtocolReachability::Unknown &&
        rank_analysis) {
      if (rank_analysis->dependsOnRank(op.inst)) {
        conditional = true;
      }
      for (const BasicBlock *pred : predecessors(op.inst->getParent())) {
        const auto *br = dyn_cast<BranchInst>(pred->getTerminator());
        if (br && br->isConditional() &&
            rank_analysis->dependsOnRank(br->getCondition())) {
          conditional = true;
          break;
        }
      }
    }
    if (conditional && seen_conditional_collectives.insert(op.inst).second) {
      state.conditional_collective_insts.push_back(op.inst);
      state.protocol_diagnostics["collective_rank_filtered"]++;
    }
  }

  for (const auto &entry : state.collective_state_by_scope) {
    const auto &key = entry.first;
    MPICollectiveProtocolState protocol_state = entry.second;
    CollectiveProtocolFrontier frontier;
    frontier.communicator_class_id = std::get<0>(key);
    frontier.communicator_subgroup_id = std::get<1>(key);
    frontier.participant_class_id = std::get<2>(key);
    frontier.protocol_class_id = std::get<3>(key);
    frontier.frontier_position = std::get<4>(key);
    frontier.frontier_id = state.protocol_frontiers.size() + 1;
    frontier.relation.kind = concurrency::RelationKind::SameCollectiveFrontier;
    frontier.relation.proof = protocol_state.relation.proof;
    frontier.relation.reason = protocol_state.relation.reason;
    if (!protocol_state.operations.empty()) {
      const auto op_it = op_index_by_inst.find(protocol_state.operations.front());
      if (op_it != op_index_by_inst.end()) {
        frontier.participants = operations[op_it->second].participant_set;
      }
    }
    frontier.transitions = protocol_state.operations;
    if (protocol_state.reachability != ProtocolReachability::AllRanks) {
      frontier.diagnostics.push_back("mpi_collective_frontier_partial_participants");
      frontier.relation.proof = concurrency::ProofStrength::May;
    }
    state.protocol_frontiers.push_back(frontier);
    state.collective_protocol_states.push_back(std::move(protocol_state));
  }

  std::map<std::tuple<size_t, size_t, size_t, size_t>,
           std::vector<const Instruction *>>
      collectives_by_frontier;
  for (const CollectiveProtocolFrontier &frontier : state.protocol_frontiers) {
    collectives_by_frontier[std::make_tuple(frontier.communicator_class_id,
                                            frontier.communicator_subgroup_id,
                                            frontier.protocol_class_id,
                                            frontier.frontier_position)] =
        frontier.transitions;
  }

  for (const auto &entry : collectives_by_frontier) {
    std::vector<MPICollectiveAnalysis::CollectiveCall> calls;
    for (const Instruction *inst : entry.second) {
      auto it = collective_call_by_inst.find(inst);
      if (it != collective_call_by_inst.end()) {
        calls.push_back(it->second);
      }
    }
    for (size_t i = 0; i < calls.size(); ++i) {
      for (size_t j = i + 1; j < calls.size(); ++j) {
        if (!areCollectivesCompatible(calls[i], calls[j])) {
          state.mismatched_collective_insts.emplace_back(calls[i].inst, calls[j].inst);
          state.protocol_diagnostics["collective_mismatch_pairs"]++;
        }
        if (calls[i].type == calls[j].type && calls[i].root_rank >= 0 &&
            calls[j].root_rank >= 0 && calls[i].root_rank != calls[j].root_rank) {
          state.wrong_root_inst_pairs.emplace_back(calls[i].inst, calls[j].inst);
        }
      }
    }
  }

  {
    std::unordered_map<size_t, const MPIChannelEndpointObligation *> endpoint_by_id;
    for (const MPIChannelEndpointObligation &endpoint : channel_endpoints) {
      endpoint_by_id[endpoint.obligation_id] = &endpoint;
    }
    std::unordered_map<size_t, const MPIRequestSetFact *> request_set_by_id;
    for (const MPIRequestSetFact &request_set : request_sets) {
      request_set_by_id[request_set.request_set_id] = &request_set;
    }
    std::unordered_map<size_t, const MPIChannelAutomaton *> automaton_by_id;
    for (const MPIChannelAutomaton &automaton : state.channel_automata) {
      automaton_by_id[automaton.channel_class_id] = &automaton;
    }

    for (MPIFunctionSummary &summary : state.function_summaries) {
      std::set<size_t> direct_op_indices(summary.expanded_operation_indices.begin(),
                                         summary.expanded_operation_indices.end());
      std::set<size_t> outstanding_send_ids;
      std::set<size_t> outstanding_recv_ids;
      std::set<size_t> outstanding_request_set_ids;
      std::set<size_t> unresolved_channel_ids;
      std::set<size_t> collective_ops;
      std::set<size_t> collective_slots;
      std::set<size_t> outstanding_frontiers;

      for (size_t channel_id : summary.touched_channel_class_ids) {
        auto automaton_it = automaton_by_id.find(channel_id);
        if (automaton_it == automaton_by_id.end()) {
          continue;
        }
        const MPIChannelAutomaton &automaton = *automaton_it->second;
        if (automaton.ambiguity_state != MPIChannelAutomaton::AmbiguityState::Unique) {
          unresolved_channel_ids.insert(channel_id);
        }
        for (size_t obligation_id : automaton.unresolved_send_obligation_ids) {
          auto endpoint_it = endpoint_by_id.find(obligation_id);
          if (endpoint_it == endpoint_by_id.end()) {
            continue;
          }
          if (direct_op_indices.count(endpoint_it->second->operation_index)) {
            outstanding_send_ids.insert(obligation_id);
          }
        }
        for (size_t obligation_id : automaton.unresolved_receive_obligation_ids) {
          auto endpoint_it = endpoint_by_id.find(obligation_id);
          if (endpoint_it == endpoint_by_id.end()) {
            continue;
          }
          if (direct_op_indices.count(endpoint_it->second->operation_index)) {
            outstanding_recv_ids.insert(obligation_id);
          }
        }
        for (size_t request_set_id : automaton.unresolved_completion_request_set_ids) {
          auto request_it = request_set_by_id.find(request_set_id);
          if (request_it == request_set_by_id.end()) {
            continue;
          }
          if (!request_it->second->transition_inst) {
            continue;
          }
          auto op_it = op_index_by_inst.find(request_it->second->transition_inst);
          if (op_it != op_index_by_inst.end() && direct_op_indices.count(op_it->second)) {
            outstanding_request_set_ids.insert(request_set_id);
          }
        }
      }

      for (size_t op_index : summary.expanded_collective_operation_indices) {
        if (op_index >= operations.size()) {
          continue;
        }
        const MPIOperation &op = operations[op_index];
        collective_ops.insert(op_index);
        collective_slots.insert(op.protocol_sequence_id);
        if (op.protocol_reachability != ProtocolReachability::AllRanks) {
          summary.unresolved_collective_summary_effect = true;
        }
      }

      for (const CollectiveProtocolFrontier &frontier : state.protocol_frontiers) {
        bool touches_summary = false;
        for (const Instruction *inst : frontier.transitions) {
          auto op_it = op_index_by_inst.find(inst);
          if (op_it != op_index_by_inst.end() && direct_op_indices.count(op_it->second)) {
            touches_summary = true;
            break;
          }
        }
        if (!touches_summary) {
          continue;
        }
        if (frontier.relation.proof != concurrency::ProofStrength::Must) {
          outstanding_frontiers.insert(frontier.frontier_id);
          summary.unresolved_collective_summary_effect = true;
        }
      }

      summary.outstanding_send_endpoint_ids.assign(outstanding_send_ids.begin(),
                                                   outstanding_send_ids.end());
      summary.outstanding_receive_endpoint_ids.assign(outstanding_recv_ids.begin(),
                                                      outstanding_recv_ids.end());
      summary.outstanding_request_set_ids.assign(outstanding_request_set_ids.begin(),
                                                 outstanding_request_set_ids.end());
      summary.unresolved_channel_class_ids.assign(unresolved_channel_ids.begin(),
                                                  unresolved_channel_ids.end());
      summary.collective_call_operation_indices.assign(collective_ops.begin(),
                                                       collective_ops.end());
      summary.entered_collective_protocol_slots.assign(collective_slots.begin(),
                                                       collective_slots.end());
      summary.outstanding_collective_frontier_ids.assign(outstanding_frontiers.begin(),
                                                         outstanding_frontiers.end());

      if (summary.unresolved_collective_summary_effect &&
          !summary.collective_call_operation_indices.empty()) {
        MPIModelGap gap;
        gap.domain = MPIModelGapDomain::CollectiveProtocol;
        gap.inst = summary.function && !summary.function->empty()
                       ? &summary.function->getEntryBlock().front()
                       : nullptr;
        gap.relation.kind = concurrency::RelationKind::UnknownDueToModelGap;
        gap.relation.proof = concurrency::ProofStrength::Unknown;
        gap.relation.reason = summary.recursive
                                  ? "mpi_collective_recursive_summary_unresolved"
                                  : "mpi_collective_summary_unresolved";
        gap.code = gap.relation.reason;
        gap.detail = summary.function ? summary.function->getName().str() : "";
        state.model_gaps.push_back(gap);
      }
    }
  }

  {
    std::unordered_map<size_t, const MPIChannelEndpointObligation *> endpoint_by_id;
    for (const MPIChannelEndpointObligation &endpoint : channel_endpoints) {
      endpoint_by_id[endpoint.obligation_id] = &endpoint;
    }
    std::set<std::pair<size_t, size_t>> obligation_pairs;
    for (const MPIChannelObligation &obligation : state.channel_obligations) {
      if (obligation.sender_obligation_id != 0 &&
          obligation.receiver_obligation_id != 0) {
        obligation_pairs.emplace(obligation.sender_obligation_id,
                                 obligation.receiver_obligation_id);
      }
      if (obligation.sender_obligation_id != 0 &&
          !endpoint_by_id.count(obligation.sender_obligation_id)) {
        emitValidationGap(state, MPIModelGapDomain::PointToPoint,
                          obligation.sender_inst,
                          "mpi_validation_missing_sender_endpoint", "builder",
                          "channel obligation references missing sender endpoint",
                          obligation.communicator_class_id, 0,
                          obligation.channel_class_id, obligation.request_set_id);
      }
      if (obligation.receiver_obligation_id != 0 &&
          !endpoint_by_id.count(obligation.receiver_obligation_id)) {
        emitValidationGap(state, MPIModelGapDomain::PointToPoint,
                          obligation.receiver_inst,
                          "mpi_validation_missing_receiver_endpoint", "builder",
                          "channel obligation references missing receiver endpoint",
                          obligation.communicator_class_id, 0,
                          obligation.channel_class_id, obligation.request_set_id);
      }
      if (!requestSetExists(state.request_set_facts, obligation.request_set_id)) {
        emitValidationGap(state, MPIModelGapDomain::Completion,
                          obligation.discharge_inst ? obligation.discharge_inst
                                                    : obligation.sender_inst,
                          "mpi_validation_missing_request_set", "builder",
                          "channel obligation references missing request set",
                          obligation.communicator_class_id, 0,
                          obligation.channel_class_id, obligation.request_set_id);
      }
    }

    for (const MPIRequestFact &fact : state.request_facts) {
      if (!requestSetExists(state.request_set_facts, fact.request_set_id)) {
        emitValidationGap(state, MPIModelGapDomain::Completion,
                          fact.last_transition_inst ? fact.last_transition_inst
                                                    : fact.origin_inst,
                          "mpi_validation_request_fact_missing_request_set",
                          "builder",
                          "request fact references missing request set",
                          fact.communicator_class_id, 0, fact.channel_class_id,
                          fact.request_set_id);
      }
      if (!channelClassExists(state.channel_automata, fact.channel_class_id)) {
        emitValidationGap(state, MPIModelGapDomain::PointToPoint,
                          fact.last_transition_inst ? fact.last_transition_inst
                                                    : fact.origin_inst,
                          "mpi_validation_request_fact_missing_channel_class",
                          "builder",
                          "request fact references missing channel automaton",
                          fact.communicator_class_id, 0, fact.channel_class_id,
                          fact.request_set_id);
      }
    }

    auto endpointExists = [&](size_t id) {
      return id == 0 || endpoint_by_id.count(id) != 0;
    };
    auto frontierExists = [&](size_t id) {
      return id == 0 ||
             std::any_of(state.protocol_frontiers.begin(),
                         state.protocol_frontiers.end(),
                         [&](const CollectiveProtocolFrontier &frontier) {
                           return frontier.frontier_id == id;
                         });
    };
    auto collectiveOpExists = [&](size_t op_index) {
      return op_index < operations.size() &&
             isCollectiveOperationKind(operations[op_index].kind);
    };

    for (const MPIChannelAutomaton &automaton : state.channel_automata) {
      for (const auto &pair : automaton.matched_endpoint_pairs) {
        if (!obligation_pairs.count(pair)) {
          emitValidationGap(state, MPIModelGapDomain::PointToPoint, nullptr,
                            "mpi_validation_unbacked_matched_pair", "builder",
                            "matched endpoint pair has no channel obligation",
                            automaton.communicator_class_id, 0,
                            automaton.channel_class_id, 0);
        }
      }
      for (size_t request_set_id : automaton.unresolved_completion_request_set_ids) {
        if (!requestSetExists(state.request_set_facts, request_set_id)) {
          emitValidationGap(state, MPIModelGapDomain::Completion, nullptr,
                            "mpi_validation_missing_unresolved_request_set",
                            "builder",
                            "automaton references missing unresolved request set",
                            automaton.communicator_class_id, 0,
                            automaton.channel_class_id, request_set_id);
        }
      }
    }

    for (const MPIFunctionSummary &summary : state.function_summaries) {
      const Instruction *anchor =
          summary.function && !summary.function->empty()
              ? &summary.function->getEntryBlock().front()
              : nullptr;
      for (size_t id : summary.outstanding_send_endpoint_ids) {
        if (!endpointExists(id)) {
          emitValidationGap(state, MPIModelGapDomain::PointToPoint, anchor,
                            "mpi_validation_summary_missing_send_endpoint",
                            "builder",
                            "function summary references missing outstanding send endpoint");
        }
      }
      for (size_t id : summary.outstanding_receive_endpoint_ids) {
        if (!endpointExists(id)) {
          emitValidationGap(state, MPIModelGapDomain::PointToPoint, anchor,
                            "mpi_validation_summary_missing_recv_endpoint",
                            "builder",
                            "function summary references missing outstanding receive endpoint");
        }
      }
      for (size_t id : summary.outstanding_request_set_ids) {
        if (!requestSetExists(state.request_set_facts, id)) {
          emitValidationGap(state, MPIModelGapDomain::Completion, anchor,
                            "mpi_validation_summary_missing_request_set",
                            "builder",
                            "function summary references missing outstanding request set");
        }
      }
      for (size_t id : summary.outstanding_collective_frontier_ids) {
        if (!frontierExists(id)) {
          emitValidationGap(state, MPIModelGapDomain::CollectiveProtocol, anchor,
                            "mpi_validation_summary_missing_collective_frontier",
                            "builder",
                            "function summary references missing collective frontier");
        }
      }
      for (size_t op_index : summary.collective_call_operation_indices) {
        if (!collectiveOpExists(op_index)) {
          emitValidationGap(state, MPIModelGapDomain::CollectiveProtocol, anchor,
                            "mpi_validation_summary_noncollective_operation",
                            "builder",
                            "function summary references non-collective operation");
        }
      }
      for (size_t op_index : summary.expanded_collective_operation_indices) {
        if (!collectiveOpExists(op_index)) {
          emitValidationGap(state, MPIModelGapDomain::CollectiveProtocol, anchor,
                            "mpi_validation_summary_noncollective_expanded_operation",
                            "builder",
                            "expanded collective summary references non-collective operation");
        }
      }
      for (size_t slot_id : summary.entered_collective_protocol_slots) {
        bool seen = std::any_of(protocol_relations.begin(), protocol_relations.end(),
                                [&](const MPICollectiveAnalysis::CollectiveCall &call) {
                                  return call.function == summary.function &&
                                         call.protocol_sequence_id == slot_id;
                                });
        if (!seen) {
          emitValidationGap(state, MPIModelGapDomain::CollectiveProtocol, anchor,
                            "mpi_validation_summary_missing_collective_slot",
                            "builder",
                            "function summary references missing protocol slot");
        }
      }
    }
  }

  {
    struct DeadlockNode {
      const Function *function = nullptr;
      const Instruction *inst = nullptr;
      std::set<size_t> send_ids;
      std::set<size_t> recv_ids;
      std::set<size_t> request_set_ids;
      bool unresolved_summary = false;
    };

    std::vector<DeadlockNode> nodes;
    std::unordered_map<const Function *, size_t> node_by_function;
    for (const MPIFunctionSummary &summary : state.function_summaries) {
      if (!summary.function) {
        continue;
      }
      if (summary.outstanding_send_endpoint_ids.empty() &&
          summary.outstanding_receive_endpoint_ids.empty() &&
          summary.outstanding_request_set_ids.empty()) {
        continue;
      }
      DeadlockNode node;
      node.function = summary.function;
      node.inst = summary.function->empty() ? nullptr
                                            : &summary.function->getEntryBlock().front();
      node.send_ids.insert(summary.outstanding_send_endpoint_ids.begin(),
                           summary.outstanding_send_endpoint_ids.end());
      node.recv_ids.insert(summary.outstanding_receive_endpoint_ids.begin(),
                           summary.outstanding_receive_endpoint_ids.end());
      node.request_set_ids.insert(summary.outstanding_request_set_ids.begin(),
                                  summary.outstanding_request_set_ids.end());
      node.unresolved_summary =
          summary.unresolved_indirect_call_effect ||
          summary.unresolved_collective_summary_effect || summary.recursive;
      node_by_function[summary.function] = nodes.size();
      nodes.push_back(std::move(node));
    }

    std::unordered_map<size_t, const MPIChannelObligation *> obligation_by_sender;
    std::unordered_map<size_t, const MPIChannelObligation *> obligation_by_receiver;
    std::unordered_map<size_t, const MPIChannelEndpointObligation *> endpoint_by_id;
    for (const MPIChannelEndpointObligation &endpoint : channel_endpoints) {
      endpoint_by_id[endpoint.obligation_id] = &endpoint;
    }
    for (const MPIChannelObligation &obligation : state.channel_obligations) {
      obligation_by_sender[obligation.sender_obligation_id] = &obligation;
      obligation_by_receiver[obligation.receiver_obligation_id] = &obligation;
    }

    struct Edge {
      size_t target = 0;
      MPICommunicationMatch proof = MPICommunicationMatch::Unknown;
    };
    std::vector<std::vector<Edge>> graph(nodes.size());
    for (size_t node_idx = 0; node_idx < nodes.size(); ++node_idx) {
      const DeadlockNode &node = nodes[node_idx];
      auto addEdge = [&](const Function *target_fn, MPICommunicationMatch proof) {
        auto target_it = node_by_function.find(target_fn);
        if (target_it == node_by_function.end() || target_it->second == node_idx) {
          return;
        }
        for (const Edge &edge : graph[node_idx]) {
          if (edge.target == target_it->second && edge.proof == proof) {
            return;
          }
        }
        graph[node_idx].push_back({target_it->second, proof});
      };

      for (size_t send_id : node.send_ids) {
        auto obligation_it = obligation_by_sender.find(send_id);
        if (obligation_it == obligation_by_sender.end() || !obligation_it->second ||
            obligation_it->second->discharged) {
          continue;
        }
        const MPIChannelObligation &obligation = *obligation_it->second;
        auto endpoint_it = endpoint_by_id.find(obligation.receiver_obligation_id);
        if (endpoint_it == endpoint_by_id.end() || !endpoint_it->second) {
          continue;
        }
        const MPIOperation &recv_op = operations[endpoint_it->second->operation_index];
        addEdge(recv_op.function, obligation.proof);
      }
      for (size_t recv_id : node.recv_ids) {
        auto obligation_it = obligation_by_receiver.find(recv_id);
        if (obligation_it == obligation_by_receiver.end() || !obligation_it->second ||
            obligation_it->second->discharged) {
          continue;
        }
        const MPIChannelObligation &obligation = *obligation_it->second;
        auto endpoint_it = endpoint_by_id.find(obligation.sender_obligation_id);
        if (endpoint_it == endpoint_by_id.end() || !endpoint_it->second) {
          continue;
        }
        const MPIOperation &send_op = operations[endpoint_it->second->operation_index];
        addEdge(send_op.function, obligation.proof);
      }
      for (size_t request_set_id : node.request_set_ids) {
        for (const MPIChannelObligation &obligation : state.channel_obligations) {
          if (obligation.request_set_id != request_set_id || obligation.discharged) {
            continue;
          }
          auto send_endpoint_it = endpoint_by_id.find(obligation.sender_obligation_id);
          auto recv_endpoint_it = endpoint_by_id.find(obligation.receiver_obligation_id);
          if (send_endpoint_it != endpoint_by_id.end() && send_endpoint_it->second) {
            const MPIOperation &send_op =
                operations[send_endpoint_it->second->operation_index];
            addEdge(send_op.function, obligation.proof);
          }
          if (recv_endpoint_it != endpoint_by_id.end() && recv_endpoint_it->second) {
            const MPIOperation &recv_op =
                operations[recv_endpoint_it->second->operation_index];
            addEdge(recv_op.function, obligation.proof);
          }
        }
      }
    }

    std::set<std::pair<const Instruction *, const Instruction *>> deadlocks;
    std::vector<int> color(nodes.size(), 0);
    std::vector<size_t> stack;
    std::function<void(size_t)> dfs = [&](size_t node_idx) {
      color[node_idx] = 1;
      stack.push_back(node_idx);
      for (const Edge &edge : graph[node_idx]) {
        if (color[edge.target] == 0) {
          dfs(edge.target);
          continue;
        }
        if (color[edge.target] != 1) {
          continue;
        }
        auto begin = std::find(stack.begin(), stack.end(), edge.target);
        if (begin == stack.end()) {
          continue;
        }
        std::vector<size_t> cycle(begin, stack.end());
        if (cycle.size() < 2) {
          continue;
        }
        bool has_must = false;
        bool all_may = true;
        bool has_exit = false;
        for (size_t member : cycle) {
          for (const Edge &member_edge : graph[member]) {
            bool inside = std::find(cycle.begin(), cycle.end(), member_edge.target) !=
                          cycle.end();
            if (inside) {
              if (member_edge.proof == MPICommunicationMatch::MustMatch) {
                has_must = true;
                all_may = false;
              } else if (member_edge.proof != MPICommunicationMatch::MayMatch) {
                all_may = false;
              }
            } else if (member_edge.proof == MPICommunicationMatch::MustMatch ||
                       member_edge.proof == MPICommunicationMatch::MayMatch) {
              has_exit = true;
            }
          }
        }
        bool unresolved = false;
        for (size_t member : cycle) {
          unresolved = unresolved || nodes[member].unresolved_summary;
        }
        if (!has_must && all_may && has_exit && !unresolved) {
          continue;
        }
        std::vector<const Instruction *> members;
        for (size_t member : cycle) {
          if (nodes[member].inst) {
            members.push_back(nodes[member].inst);
          }
        }
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());
        if (members.size() < 2) {
          continue;
        }
        for (size_t idx = 0; idx < members.size(); ++idx) {
          const Instruction *lhs = members[idx];
          const Instruction *rhs = members[(idx + 1) % members.size()];
          deadlocks.emplace(lhs < rhs ? lhs : rhs, lhs < rhs ? rhs : lhs);
        }
      }
      stack.pop_back();
      color[node_idx] = 2;
    };
    for (size_t idx = 0; idx < nodes.size(); ++idx) {
      if (color[idx] == 0) {
        dfs(idx);
      }
    }
    state.potential_deadlocks = process_model_.findPotentialDeadlocks();
  }

  state.rma_synchronization_facts = rma_facts;
  state.invalid_epoch_transitions = rma_analysis_.findInvalidEpochTransitions();
  state.use_after_free_windows = rma_analysis_.findUseAfterFreeWindows();
  state.double_window_free = rma_analysis_.findDoubleWindowFree();
  state.leaked_windows = rma_analysis_.findLeakedWindows();

  std::set<WindowID> tracked_windows;
  for (const auto &relation : rma_relations) {
    if (relation.window) {
      tracked_windows.insert(relation.window);
    }
  }
  state.tracked_window_count = tracked_windows.size();

  for (const RMASynchronizationFact &fact : rma_facts) {
    MPIRMAEpochFact &epoch_fact =
        state.upsertRMAEpochFact(fact.participant_class_id, fact.window, fact.epoch_id);
    epoch_fact.window = fact.window;
    epoch_fact.epoch_id = fact.epoch_id;
    epoch_fact.participant_class_id = fact.participant_class_id;
    epoch_fact.participants = fact.participants;
    epoch_fact.sync_kind = fact.sync_kind;
    epoch_fact.completion = fact.completion;
    epoch_fact.relation = fact.relation;
    auto relation_it = rma_relation_by_inst.find(fact.inst);
    if (relation_it != rma_relation_by_inst.end()) {
      epoch_fact.sync_model = relation_it->second.sync_model == MPIRMAAnalysis::SyncModel::FENCE
                                  ? MPIRMASyncModel::Fence
                                  : (relation_it->second.sync_model ==
                                             MPIRMAAnalysis::SyncModel::LOCK_UNLOCK
                                         ? MPIRMASyncModel::LockUnlock
                                         : (relation_it->second.sync_model ==
                                                    MPIRMAAnalysis::SyncModel::PSCW
                                                ? MPIRMASyncModel::PSCW
                                                : MPIRMASyncModel::None));
      epoch_fact.sync_start = relation_it->second.sync_start;
      epoch_fact.sync_end = relation_it->second.sync_end;
    }
    epoch_fact.operations.push_back(fact.inst);
  }
  for (const auto &entry : state.rma_epoch_by_key) {
    state.rma_epoch_facts.push_back(entry.second);
  }

  std::set<const Instruction *> synchronized_rma;
  for (const RMASynchronizationFact &fact : rma_facts) {
    if (fact.access_kind == MPIRMAAccessKind::None) {
      continue;
    }
    if (fact.relation.kind == concurrency::RelationKind::SameSynchronizationEpoch ||
        fact.relation.kind ==
            concurrency::RelationKind::LocalOnlySynchronizationCompletion) {
      synchronized_rma.insert(fact.inst);
    }
  }
  for (const auto &relation : rma_relations) {
    if (!synchronized_rma.count(relation.inst) ||
        relation.sync_model == MPIRMAAnalysis::SyncModel::NONE) {
      state.unsynchronized_rma_insts.push_back(relation.inst);
    }
  }

  ThreadAPI *thread_api = ThreadAPI::getThreadAPI();
  auto raceFactsConflict = [&](const MPIRMAAnalysis::RMAOperation &lhs,
                               const MPIRMAAnalysis::RMAOperation &rhs) {
    auto lhs_fact_it = rma_fact_by_inst.find(lhs.inst);
    auto rhs_fact_it = rma_fact_by_inst.find(rhs.inst);
    const RMASynchronizationFact *lhs_fact =
        lhs_fact_it == rma_fact_by_inst.end() ? nullptr : &lhs_fact_it->second;
    const RMASynchronizationFact *rhs_fact =
        rhs_fact_it == rma_fact_by_inst.end() ? nullptr : &rhs_fact_it->second;

    if (lhs.window != rhs.window) {
      return false;
    }
    if (lhs_fact && rhs_fact &&
        !participantsMayOverlap(lhs_fact->participants, rhs_fact->participants)) {
      return false;
    }

    const int lhs_target = lhs_fact ? lhs_fact->target_rank : lhs.target_rank;
    const int rhs_target = rhs_fact ? rhs_fact->target_rank : rhs.target_rank;
    const int lhs_target_min = lhs_fact ? lhs_fact->target_rank_min : lhs.target_rank_min;
    const int lhs_target_max = lhs_fact ? lhs_fact->target_rank_max : lhs.target_rank_max;
    const int rhs_target_min = rhs_fact ? rhs_fact->target_rank_min : rhs.target_rank_min;
    const int rhs_target_max = rhs_fact ? rhs_fact->target_rank_max : rhs.target_rank_max;
    if (!rangesOverlap(lhs_target, lhs_target, rhs_target, rhs_target) &&
        !rangesOverlap(lhs_target_min, lhs_target_max, rhs_target_min,
                       rhs_target_max)) {
      return false;
    }

    const int64_t lhs_disp = lhs_fact ? lhs_fact->target_disp : lhs.target_disp;
    const int64_t rhs_disp = rhs_fact ? rhs_fact->target_disp : rhs.target_disp;
    const int64_t lhs_len =
        lhs_fact && lhs_fact->byte_length > 0
            ? lhs_fact->byte_length
            : (lhs.byte_length > 0 ? lhs.byte_length : 1);
    const int64_t rhs_len =
        rhs_fact && rhs_fact->byte_length > 0
            ? rhs_fact->byte_length
            : (rhs.byte_length > 0 ? rhs.byte_length : 1);
    if (lhs_disp != -1 && rhs_disp != -1) {
      int64_t lhs_end = lhs_disp + lhs_len;
      int64_t rhs_end = rhs_disp + rhs_len;
      if (!(lhs_disp < rhs_end && rhs_disp < lhs_end)) {
        return false;
      }
    }

    const Function *lhs_callee = thread_api->getCallee(lhs.inst);
    const Function *rhs_callee = thread_api->getCallee(rhs.inst);
    ThreadAPI::TD_TYPE lhs_type =
        lhs_callee ? thread_api->getType(lhs_callee) : ThreadAPI::TD_DUMMY;
    ThreadAPI::TD_TYPE rhs_type =
        rhs_callee ? thread_api->getType(rhs_callee) : ThreadAPI::TD_DUMMY;
    MPIRMAAccessKind lhs_access =
        lhs_fact ? lhs_fact->access_kind
                 : (lhs_type == ThreadAPI::TD_MPI_GET ? MPIRMAAccessKind::Get
                                                      : MPIRMAAccessKind::Put);
    MPIRMAAccessKind rhs_access =
        rhs_fact ? rhs_fact->access_kind
                 : (rhs_type == ThreadAPI::TD_MPI_GET ? MPIRMAAccessKind::Get
                                                      : MPIRMAAccessKind::Put);
    if (!isRMAWriteAccess(lhs_access) && !isRMAWriteAccess(rhs_access)) {
      return false;
    }

    if (lhs_fact && rhs_fact) {
      if (lhs_fact->completion == MPIRMACompletionStrength::Remote &&
          rhs_fact->completion == MPIRMACompletionStrength::Remote &&
          lhs_fact->epoch_id != 0 && lhs_fact->epoch_id == rhs_fact->epoch_id) {
        return false;
      }
      if (lhs_fact->completion == MPIRMACompletionStrength::Local ||
          rhs_fact->completion == MPIRMACompletionStrength::Local) {
        return true;
      }
      if (lhs_fact->code == "mpi_rma_pscw_group_unresolved" ||
          rhs_fact->code == "mpi_rma_pscw_group_unresolved") {
        return true;
      }
    }

    if (lhs.sync_model == MPIRMAAnalysis::SyncModel::NONE ||
        rhs.sync_model == MPIRMAAnalysis::SyncModel::NONE) {
      return true;
    }
    if (lhs.sync_model != rhs.sync_model || lhs.local_completion_only ||
        rhs.local_completion_only) {
      return true;
    }
    return !(lhs.epoch_id != 0 && lhs.epoch_id == rhs.epoch_id);
  };

  for (size_t i = 0; i < rma_relations.size(); ++i) {
    for (size_t j = i + 1; j < rma_relations.size(); ++j) {
      if (raceFactsConflict(rma_relations[i], rma_relations[j])) {
        state.rma_race_insts.emplace_back(rma_relations[i].inst, rma_relations[j].inst);
      }
    }
  }

  for (const auto &entry : state.communicator_fact_by_class) {
    state.communicator_facts.push_back(entry.second);
  }

  return state;
}

} // namespace mpi
