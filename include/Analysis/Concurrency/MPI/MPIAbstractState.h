#pragma once

#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/MPI/MPISemanticEvent.h"

#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace mpi {

enum class MPICommunicatorCreationKind {
  Unknown,
  World,
  Dup,
  Split,
  Create,
  IntercommCreate,
  Topology,
  Session
};

enum class MPIRequestFactKind {
  Unknown,
  PointToPoint,
  Collective,
  Persistent
};

struct MPICommunicatorFact {
  CommunicatorID canonical = nullptr;
  CommunicatorID parent = nullptr;
  size_t communicator_class_id = 0;
  size_t subgroup_id = 0;
  MPICommunicatorCreationKind creation_kind =
      MPICommunicatorCreationKind::Unknown;
  MPICommunicatorSubgroupTokenKind subgroup_token_kind =
      MPICommunicatorSubgroupTokenKind::None;
  MPIProcessSetScopeKind participant_scope = MPIProcessSetScopeKind::Unknown;
  MPICommunicatorSide communicator_side = MPICommunicatorSide::Unknown;
  MPIParticipantSet subgroup;
  bool is_intercommunicator = false;
  bool has_known_size = false;
  int size_min = -1;
  int size_max = -1;
  std::string topology_kind;
  std::string detail;
};

struct MPIRequestFact {
  RequestID request = nullptr;
  MPIRequestFactKind kind = MPIRequestFactKind::Unknown;
  size_t channel_class_id = 0;
  size_t request_set_id = 0;
  const llvm::Instruction *origin_inst = nullptr;
  const llvm::Instruction *activation_inst = nullptr;
  const llvm::Instruction *last_transition_inst = nullptr;
  MPIRequestState state = MPIRequestState::Unknown;
  MPIRequestCompletionScopeKind completion_scope =
      MPIRequestCompletionScopeKind::Unknown;
  bool is_persistent = false;
  bool is_collective = false;
  int peer_rank = -1;
  int tag = -1;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  MPISendMode send_mode = MPISendMode::Unknown;
  concurrency::Relation relation;
  std::string provenance;
};

struct MPIChannelTransition {
  enum class Kind {
    Unknown,
    PostSend,
    PostReceive,
    CandidateMatch,
    UniqueMatch,
    DischargeByWait,
    DischargeByTest,
    ReleaseByCancelOrFree
  };

  size_t operation_index = 0;
  size_t endpoint_obligation_id = 0;
  size_t request_set_id = 0;
  const llvm::Instruction *inst = nullptr;
  bool is_send = false;
  bool is_recv = false;
  bool blocking = false;
  bool discharged = false;
  Kind kind = Kind::Unknown;
  MPICommunicationMatch proof = MPICommunicationMatch::Unknown;
  std::string proof_source;
  concurrency::Relation relation;
};

struct MPIChannelAutomaton {
  enum class AmbiguityState { Unique, NonUnique, UnresolvedIdentity };

  size_t channel_class_id = 0;
  size_t communicator_class_id = 0;
  MPIParticipantSet sender_set;
  MPIParticipantSet receiver_set;
  int tag = -1;
  MPITagClassKind tag_class = MPITagClassKind::Wildcard;
  MPISendMode send_mode = MPISendMode::Unknown;
  int64_t datatype_size_class = -1;
  bool has_wildcard_endpoint = false;
  size_t posted_receive_count = 0;
  size_t inflight_send_count = 0;
  size_t unresolved_obligation_count = 0;
  std::vector<size_t> posted_send_obligation_ids;
  std::vector<size_t> posted_receive_obligation_ids;
  std::vector<std::pair<size_t, size_t>> matched_endpoint_pairs;
  std::vector<size_t> unresolved_send_obligation_ids;
  std::vector<size_t> unresolved_receive_obligation_ids;
  std::vector<size_t> unresolved_completion_request_set_ids;
  AmbiguityState ambiguity_state = AmbiguityState::Unique;
  std::vector<MPIChannelTransition> transitions;
  std::vector<MPIChannelObligation> obligations;
};

struct MPICollectiveProtocolState {
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t participant_class_id = 0;
  size_t protocol_class_id = 0;
  size_t protocol_position = 0;
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  MPICollectiveVariant variant = MPICollectiveVariant::Unknown;
  MPICollectiveShape shape = MPICollectiveShape::Unknown;
  ProtocolReachability reachability = ProtocolReachability::Unknown;
  std::vector<const llvm::Instruction *> operations;
  concurrency::Relation relation;
};

struct MPIRMAEpochFact {
  WindowID window = nullptr;
  size_t epoch_id = 0;
  size_t participant_class_id = 0;
  MPIParticipantSet participants;
  MPIRMASyncKind sync_kind = MPIRMASyncKind::None;
  MPIRMASyncModel sync_model = MPIRMASyncModel::None;
  MPIRMACompletionStrength completion = MPIRMACompletionStrength::None;
  const llvm::Instruction *sync_start = nullptr;
  const llvm::Instruction *sync_end = nullptr;
  std::vector<const llvm::Instruction *> operations;
  concurrency::Relation relation;
};

struct MPIFunctionSummary {
  const llvm::Function *function = nullptr;
  std::vector<const llvm::Function *> callees;
  std::vector<size_t> direct_operation_indices;
  // Debugging/fallback projection only. Do not treat as semantic truth when a
  // summary-owned effect state is available.
  std::vector<size_t> expanded_operation_indices;
  std::vector<size_t> collective_operation_indices;
  std::vector<size_t> expanded_collective_operation_indices;
  std::vector<size_t> request_operation_indices;
  std::vector<size_t> channel_operation_indices;
  std::vector<size_t> rma_operation_indices;
  std::vector<size_t> communicator_class_ids;
  std::vector<size_t> emitted_send_endpoint_ids;
  std::vector<size_t> emitted_receive_endpoint_ids;
  std::vector<size_t> created_request_set_ids;
  std::vector<size_t> discharged_request_set_ids;
  std::vector<size_t> touched_channel_class_ids;
  std::vector<size_t> outstanding_send_endpoint_ids;
  std::vector<size_t> outstanding_receive_endpoint_ids;
  std::vector<size_t> outstanding_request_set_ids;
  std::vector<size_t> unresolved_channel_class_ids;
  std::vector<size_t> blocking_endpoint_obligation_ids;
  std::vector<size_t> blocking_request_set_ids;
  std::vector<size_t> collective_call_operation_indices;
  std::vector<size_t> entered_collective_protocol_slots;
  std::vector<size_t> outstanding_collective_frontier_ids;
  bool recursive = false;
  bool reaches_fixed_point = false;
  bool unresolved_indirect_call_effect = false;
  bool unresolved_collective_summary_effect = false;
  size_t iterations = 0;
};

struct MPIAbstractState {
  std::map<size_t, MPICommunicatorFact> communicator_fact_by_class;
  std::map<RequestID, MPIRequestFact> request_fact_by_request;
  std::map<std::string, MPIChannelAutomaton> channel_automata_by_key;
  std::map<std::tuple<size_t, size_t, size_t, size_t, size_t>,
           MPICollectiveProtocolState>
      collective_state_by_scope;
  std::map<std::tuple<size_t, WindowID, size_t>, MPIRMAEpochFact>
      rma_epoch_by_key;
  std::unordered_map<std::string, size_t> protocol_diagnostics;

  std::vector<MPICommunicatorFact> communicator_facts;
  std::vector<MPIFunctionSummary> function_summaries;
  std::vector<MPIRequestFact> request_facts;
  std::vector<MPIRequestSetFact> request_set_facts;
  std::vector<MPIChannelAutomaton> channel_automata;
  std::vector<MPICollectiveProtocolState> collective_protocol_states;
  std::vector<MPIRMAEpochFact> rma_epoch_facts;
  std::vector<MPIProcessSetFact> process_set_facts;
  std::vector<MPIParticipantSet> participant_sets;
  std::vector<MPIChannelObligation> channel_obligations;
  std::vector<CollectiveProtocolFrontier> protocol_frontiers;
  std::vector<RMASynchronizationFact> rma_synchronization_facts;
  std::vector<MPIModelGap> model_gaps;
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      potential_deadlocks;
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      mismatched_collective_insts;
  std::vector<const llvm::Instruction *> conditional_collective_insts;
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      wrong_root_inst_pairs;
  std::vector<const llvm::Instruction *> unsynchronized_rma_insts;
  std::vector<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      rma_race_insts;
  std::vector<WindowID> leaked_windows;
  std::vector<const llvm::Instruction *> invalid_epoch_transitions;
  std::vector<const llvm::Instruction *> use_after_free_windows;
  std::vector<const llvm::Instruction *> double_window_free;
  size_t tracked_window_count = 0;

  void clear();
  MPICommunicatorFact &upsertCommunicatorFact(size_t communicator_class_id);
  MPIRequestFact &upsertRequestFact(RequestID request);
  MPIChannelAutomaton &upsertChannelAutomaton(const std::string &key);
  MPICollectiveProtocolState &
  upsertCollectiveState(size_t communicator_class_id,
                        size_t communicator_subgroup_id,
                        size_t participant_class_id, size_t protocol_class_id,
                        size_t protocol_position);
  MPIRMAEpochFact &upsertRMAEpochFact(size_t participant_class_id, WindowID window,
                                      size_t epoch_id);
};

class MPIProcessModel;
class MPICollectiveAnalysis;
class MPIRMAAnalysis;

// MPIAbstractStateBuilder is the semantic owner of executed automata/summary
// state. Public request/channel/collective facts are projections built here
// from normalized emitter facts plus collective/RMA analyses.
class MPIAbstractStateBuilder {
public:
  MPIAbstractStateBuilder(llvm::Module &module, const MPIProcessModel &process_model,
                          const MPICollectiveAnalysis &collective_analysis,
                          const MPIRMAAnalysis &rma_analysis)
      : module_(module), process_model_(process_model),
        collective_analysis_(collective_analysis), rma_analysis_(rma_analysis) {}

  MPIAbstractState build() const;

private:
  llvm::Module &module_;
  const MPIProcessModel &process_model_;
  const MPICollectiveAnalysis &collective_analysis_;
  const MPIRMAAnalysis &rma_analysis_;
};

} // namespace mpi
