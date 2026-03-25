/**
 * @file MPIOperation.h
 * @brief MPI Operation Types and Structures
 *
 * This file defines the core types, enums, and structures used for
 * MPI program analysis.
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#ifndef MPI_OPERATION_H
#define MPI_OPERATION_H

#include "Analysis/Concurrency/ConcurrencyRelation.h"
#include "Analysis/Concurrency/MPI/MPINormalization.h"
#include "Analysis/Concurrency/MPI/MPIRankAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace mpi {

using ProcessID = int;
using CommunicatorID = const llvm::Value *;
using RequestID = const llvm::Value *;
using WindowID = const llvm::Value *;
using GroupID = const llvm::Value *;

enum class MPICommunicationMatch { NoMatch, MustMatch, MayMatch, Unknown };

enum class MPIRequestState {
  Unbound,
  PersistentTemplate,
  InactivePersistent,
  Active,
  MayComplete,
  MustComplete,
  Canceled,
  Freed,
  Escaped,
  Unknown,
  // Compatibility aliases for older callers/tests.
  Created = PersistentTemplate,
  Pending = Active,
  Terminal = Canceled,
  Consumed = MustComplete
};

using RequestCompletionState = MPIRequestState;

enum class ProtocolReachability { AllRanks, SomeRanks, Unknown };

enum class RMAEpochKind { None, Access, Exposure };

enum class MPISendMode { Standard, Synchronous, Buffered, Ready, Unknown };

enum class MPIBlockingMode {
  Blocking,
  NonBlocking,
  Completion,
  LocalCompletion,
  Unknown
};

enum class MPIRequestArity { None, Single, Array };

enum class MPICollectiveVariant {
  Unknown,
  Barrier,
  Bcast,
  Gather,
  Gatherv,
  Scatter,
  Scatterv,
  Allgather,
  Allgatherv,
  Alltoall,
  Alltoallv,
  Alltoallw,
  Reduce,
  Allreduce,
  ReduceScatter,
  ReduceScatterBlock,
  Scan,
  Exscan,
  NeighborAllgather,
  NeighborAllgatherv,
  NeighborAlltoall,
  NeighborAlltoallv,
  NeighborAlltoallw,
  IntercommBcast
};

enum class MPICollectiveShape {
  Unknown,
  Barrier,
  Rooted,
  AllToAll,
  Reduction,
  Scan,
  Neighbor,
  Intercommunicator
};

enum class MPIProcessSetScopeKind {
  Unknown,
  All,
  ExactRank,
  RankRange,
  AllExcept
};

enum class MPICommunicatorSubgroupTokenKind {
  None,
  SplitColorConst,
  SplitColorUnknown,
  CreateGroup,
  Intercomm
};

enum class MPICommunicatorSide { Unknown, Local, Remote, Both };

enum class MPITagClassKind { Exact, Wildcard };

enum class MPIRequestCompletionScopeKind {
  None,
  Single,
  OneOfSet,
  SubsetOfSet,
  AllOfSet,
  Unknown
};

enum class MPIChannelEndpointKind { Send, Receive };

enum class MPIRequestSetKind {
  Unknown,
  PointToPoint,
  Collective,
  Persistent
};

enum class MPIRMAAccessKind { None, Put, Get, Accumulate, Atomic };

enum class MPIRMASyncKind {
  None,
  Fence,
  Lock,
  LockAll,
  Unlock,
  UnlockAll,
  Flush,
  FlushAll,
  FlushLocal,
  FlushLocalAll,
  Sync,
  PSCWPost,
  PSCWStart,
  PSCWComplete,
  PSCWWait,
  PSCWTest
};

enum class MPIRMACompletionStrength { None, Local, Remote };

enum class MPIModelGapDomain {
  None,
  Rank,
  ParticipantSet,
  Communicator,
  CollectiveProtocol,
  PointToPoint,
  RequestLifecycle,
  RMAEpoch,
  Completion,
  Unknown
};

enum class MPIOpKind {
  INIT,
  FINALIZE,
  SESSION,
  SEND_BLOCKING,
  RECV_BLOCKING,
  PROBE_BLOCKING,
  SEND_NONBLOCKING,
  RECV_NONBLOCKING,
  PROBE_NONBLOCKING,
  WAIT,
  TEST,
  BARRIER_BLOCKING,
  BARRIER_NONBLOCKING,
  COLLECTIVE_BLOCKING,
  COLLECTIVE_NONBLOCKING,
  RMA_WINDOW,
  RMA_DATA,
  RMA_SYNC,
  COMM_MANAGEMENT,
  INTERCOMM_CREATION,
  REQUEST_MANAGEMENT,
  DATATYPE_CREATE,
  UNKNOWN
};

struct MPIProcessSetFact {
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t subgroup_id = 0;
  MPICommunicatorSubgroupTokenKind subgroup_token_kind =
      MPICommunicatorSubgroupTokenKind::None;
  MPICommunicatorSide communicator_side = MPICommunicatorSide::Unknown;
  MPIProcessSetScopeKind scope_kind = MPIProcessSetScopeKind::Unknown;
  bool unknown = true;
  bool universal = false;
  int min_rank = 0;
  int max_rank = -1;
  std::set<int> excluded_ranks;
  size_t predicate_class_id = 0;
  size_t participant_class_id = 0;
  std::string provenance;
  std::string detail;

  static MPIProcessSetScopeKind
  classifyScope(bool unknown, bool universal, int min_rank, int max_rank,
                const std::set<int> &excluded_ranks) {
    if (unknown) {
      return MPIProcessSetScopeKind::Unknown;
    }
    if (universal && excluded_ranks.empty() && max_rank < 0) {
      return MPIProcessSetScopeKind::All;
    }
    if (!universal && excluded_ranks.empty() && min_rank == max_rank) {
      return MPIProcessSetScopeKind::ExactRank;
    }
    if (universal && !excluded_ranks.empty()) {
      return MPIProcessSetScopeKind::AllExcept;
    }
    return MPIProcessSetScopeKind::RankRange;
  }

  static MPIProcessSetFact
  fromPredicate(const MPI::MPIRankPredicate &predicate, size_t communicator_class,
                size_t subgroup, size_t predicate_class,
                size_t participant_class,
                MPICommunicatorSubgroupTokenKind subgroup_kind =
                    MPICommunicatorSubgroupTokenKind::None,
                MPICommunicatorSide side = MPICommunicatorSide::Unknown,
                llvm::StringRef provenance = "rank-predicate") {
    MPIProcessSetFact fact;
    fact.communicator = predicate.communicator;
    fact.communicator_class_id = communicator_class;
    fact.subgroup_id = subgroup;
    fact.subgroup_token_kind = subgroup_kind;
    fact.communicator_side = side;
    fact.unknown = predicate.unknown;
    fact.universal = predicate.universal;
    fact.min_rank = predicate.min_rank;
    fact.max_rank = predicate.max_rank;
    fact.excluded_ranks = predicate.excluded_ranks;
    fact.predicate_class_id = predicate_class;
    fact.participant_class_id = participant_class;
    fact.scope_kind = classifyScope(fact.unknown, fact.universal, fact.min_rank,
                                    fact.max_rank, fact.excluded_ranks);
    fact.provenance = provenance.str();
    return fact;
  }
};

struct MPIParticipantSet {
  CommunicatorID communicator = nullptr;
  bool unknown = true;
  bool universal = false;
  int min_rank = 0;
  int max_rank = -1;
  std::set<int> excluded_ranks;
  MPIProcessSetScopeKind scope_kind = MPIProcessSetScopeKind::Unknown;
  size_t subgroup_id = 0;
  MPICommunicatorSubgroupTokenKind subgroup_token_kind =
      MPICommunicatorSubgroupTokenKind::None;
  MPICommunicatorSide communicator_side = MPICommunicatorSide::Unknown;
  size_t predicate_class_id = 0;
  size_t participant_class_id = 0;
  std::string provenance;

  static MPIParticipantSet fromPredicate(const MPI::MPIRankPredicate &predicate,
                                         size_t predicate_class,
                                         size_t participant_class) {
    return fromProcessSetFact(MPIProcessSetFact::fromPredicate(
        predicate, 0, 0, predicate_class, participant_class));
  }

  static MPIParticipantSet fromProcessSetFact(const MPIProcessSetFact &fact) {
    MPIParticipantSet set;
    set.communicator = fact.communicator;
    set.unknown = fact.unknown;
    set.universal = fact.universal;
    set.min_rank = fact.min_rank;
    set.max_rank = fact.max_rank;
    set.excluded_ranks = fact.excluded_ranks;
    set.scope_kind = fact.scope_kind;
    set.subgroup_id = fact.subgroup_id;
    set.subgroup_token_kind = fact.subgroup_token_kind;
    set.communicator_side = fact.communicator_side;
    set.predicate_class_id = fact.predicate_class_id;
    set.participant_class_id = fact.participant_class_id;
    set.provenance = fact.provenance;
    return set;
  }

  bool constrainsParticipants() const {
    return scope_kind != MPIProcessSetScopeKind::Unknown &&
           scope_kind != MPIProcessSetScopeKind::All;
  }

  bool contains(int rank) const {
    if (unknown) {
      return true;
    }
    if (rank < min_rank) {
      return false;
    }
    if (max_rank >= 0 && rank > max_rank) {
      return false;
    }
    return excluded_ranks.count(rank) == 0;
  }

  bool mayOverlap(const MPIParticipantSet &other) const {
    if (unknown || other.unknown) {
      return true;
    }
    if (communicator && other.communicator && communicator != other.communicator) {
      return false;
    }
    if (subgroup_id != 0 && other.subgroup_id != 0 && subgroup_id != other.subgroup_id &&
        subgroup_token_kind != MPICommunicatorSubgroupTokenKind::None &&
        other.subgroup_token_kind != MPICommunicatorSubgroupTokenKind::None &&
        subgroup_token_kind != MPICommunicatorSubgroupTokenKind::SplitColorUnknown &&
        other.subgroup_token_kind != MPICommunicatorSubgroupTokenKind::SplitColorUnknown) {
      return false;
    }
    const int lhs_upper = max_rank >= 0 ? max_rank : other.max_rank;
    const int rhs_upper = other.max_rank >= 0 ? other.max_rank : max_rank;
    const int overlap_min = std::max(min_rank, other.min_rank);
    const int overlap_max =
        std::min(lhs_upper >= 0 ? lhs_upper : overlap_min + 1024,
                 rhs_upper >= 0 ? rhs_upper : overlap_min + 1024);
    if (overlap_min > overlap_max) {
      return false;
    }
    for (int rank = overlap_min; rank <= overlap_max; ++rank) {
      if (contains(rank) && other.contains(rank)) {
        return true;
      }
    }
    return universal || other.universal;
  }

  bool mustEqual(const MPIParticipantSet &other) const {
    return communicator == other.communicator && unknown == other.unknown &&
           universal == other.universal && min_rank == other.min_rank &&
           max_rank == other.max_rank && scope_kind == other.scope_kind &&
           subgroup_id == other.subgroup_id &&
           subgroup_token_kind == other.subgroup_token_kind &&
           excluded_ranks == other.excluded_ranks;
  }

  std::string toKey() const {
    if (unknown) {
      return "participants:unknown";
    }
    std::string key = "participants";
    key += ":scope=" + std::to_string(static_cast<int>(scope_kind));
    key += ":min=" + std::to_string(min_rank);
    key += ":max=" + std::to_string(max_rank);
    key += ":subgroup=" + std::to_string(subgroup_id);
    key += ":subgroup-kind=" +
           std::to_string(static_cast<int>(subgroup_token_kind));
    key += ":predicate=" + std::to_string(predicate_class_id);
    key += ":participant=" + std::to_string(participant_class_id);
    if (!excluded_ranks.empty()) {
      key += ":exclude=";
      bool first = true;
      for (int rank : excluded_ranks) {
        if (!first) {
          key += ",";
        }
        first = false;
        key += std::to_string(rank);
      }
    }
    return key;
  }
};

struct MPIModelGap {
  MPIModelGapDomain domain = MPIModelGapDomain::None;
  const llvm::Instruction *inst = nullptr;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t subgroup_id = 0;
  size_t participant_class_id = 0;
  size_t channel_class_id = 0;
  size_t request_set_id = 0;
  concurrency::Relation relation;
  std::string code;
  std::string provenance;
  std::string detail;
};

struct MPIChannelEndpointObligation {
  size_t obligation_id = 0;
  size_t operation_index = 0;
  const llvm::Instruction *inst = nullptr;
  size_t communicator_class_id = 0;
  size_t channel_class_id = 0;
  MPIChannelEndpointKind endpoint_kind = MPIChannelEndpointKind::Send;
  MPIParticipantSet participants;
  int peer_rank = -1;
  int peer_rank_min = -1;
  int peer_rank_max = -1;
  int tag = -1;
  MPITagClassKind tag_class = MPITagClassKind::Wildcard;
  int64_t datatype_size = -1;
  MPISendMode send_mode = MPISendMode::Unknown;
  RequestID request = nullptr;
  bool blocking = false;
  bool communicator_resolved = false;
  std::vector<size_t> candidate_ids;
};

struct MPIRequestSetFact {
  size_t request_set_id = 0;
  size_t communicator_class_id = 0;
  size_t channel_class_id = 0;
  std::vector<RequestID> requests;
  MPIRequestArity arity = MPIRequestArity::None;
  MPIRequestSetKind kind = MPIRequestSetKind::Unknown;
  MPIRequestState state = MPIRequestState::Unknown;
  MPIRequestCompletionScopeKind completion_scope =
      MPIRequestCompletionScopeKind::Unknown;
  const llvm::Instruction *origin_inst = nullptr;
  const llvm::Instruction *transition_inst = nullptr;
  concurrency::Relation relation;
  std::string provenance;
};

struct MPIChannelObligation {
  size_t channel_class_id = 0;
  size_t sender_obligation_id = 0;
  size_t receiver_obligation_id = 0;
  size_t request_set_id = 0;
  size_t lhs_operation_index = 0;
  size_t rhs_operation_index = 0;
  size_t sender_operation_index = 0;
  size_t receiver_operation_index = 0;
  const llvm::Instruction *lhs_inst = nullptr;
  const llvm::Instruction *rhs_inst = nullptr;
  const llvm::Instruction *sender_inst = nullptr;
  const llvm::Instruction *receiver_inst = nullptr;
  size_t communicator_class_id = 0;
  MPIParticipantSet sender_set;
  MPIParticipantSet receiver_set;
  int tag = -1;
  MPITagClassKind tag_class = MPITagClassKind::Wildcard;
  int64_t send_datatype_size = -1;
  int64_t recv_datatype_size = -1;
  MPISendMode send_mode = MPISendMode::Unknown;
  RequestID request = nullptr;
  RequestID sender_request = nullptr;
  RequestID receiver_request = nullptr;
  bool send_is_blocking = false;
  bool recv_is_blocking = false;
  bool discharged = false;
  const llvm::Instruction *discharge_inst = nullptr;
  MPICommunicationMatch proof = MPICommunicationMatch::Unknown;
  std::string proof_source;
  concurrency::Relation relation;
};

struct CollectiveProtocolFrontier {
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t participant_class_id = 0;
  size_t protocol_class_id = 0;
  size_t frontier_id = 0;
  size_t frontier_position = 0;
  MPIParticipantSet participants;
  concurrency::Relation relation;
  std::vector<const llvm::Instruction *> transitions;
  std::vector<std::string> diagnostics;
};

struct RMASynchronizationFact {
  const llvm::Instruction *inst = nullptr;
  WindowID window = nullptr;
  GroupID group = nullptr;
  size_t participant_class_id = 0;
  MPIParticipantSet participants;
  int target_rank = -1;
  int target_rank_min = -1;
  int target_rank_max = -1;
  int64_t target_disp = -1;
  int64_t byte_length = -1;
  MPIRMAAccessKind access_kind = MPIRMAAccessKind::None;
  MPIRMASyncKind sync_kind = MPIRMASyncKind::None;
  RMAEpochKind access_epoch_kind = RMAEpochKind::None;
  RMAEpochKind exposure_epoch_kind = RMAEpochKind::None;
  size_t epoch_id = 0;
  MPIRMACompletionStrength completion = MPIRMACompletionStrength::None;
  concurrency::Relation relation;
  std::string code;
};

struct MPIOperation {
  const llvm::Instruction *inst;
  MPIOpKind kind;
  ThreadAPI::TD_TYPE td_type;

  const llvm::Function *function;
  NormalizationConfidence normalization_confidence =
      NormalizationConfidence::UnknownVendorInternal;
  CommunicatorID communicator = nullptr;
  size_t communicator_class_id = 0;
  size_t communicator_subgroup_id = 0;
  size_t predicate_class_id = 0;
  size_t participant_class_id = 0;
  size_t collective_protocol_class_id = 0;
  size_t protocol_sequence_id = 0;
  size_t channel_class_id = 0;
  ProtocolReachability protocol_reachability = ProtocolReachability::Unknown;
  concurrency::Relation semantic_relation;
  MPI::RankExpr process_rank;
  MPI::MPIRankPredicate rank_predicate;
  MPIProcessSetFact process_set_fact;
  MPIParticipantSet participant_set;
  std::string rank_path_summary;
  bool is_intercommunicator = false;
  bool request_lifecycle_issue_nonblocking = false;

  MPISendMode send_mode = MPISendMode::Unknown;
  MPIBlockingMode blocking_mode = MPIBlockingMode::Unknown;
  MPIRequestArity request_arity = MPIRequestArity::None;
  MPICollectiveVariant collective_variant = MPICollectiveVariant::Unknown;
  MPICollectiveShape collective_shape = MPICollectiveShape::Unknown;
  MPIRMAAccessKind rma_access_kind = MPIRMAAccessKind::None;
  MPIRMASyncKind rma_sync_kind = MPIRMASyncKind::None;

  int source_rank = -1;
  int dest_rank = -1;
  int tag = -1;
  int source_rank_min = -1;
  int source_rank_max = -1;
  int dest_rank_min = -1;
  int dest_rank_max = -1;

  RequestID request = nullptr;
  const llvm::Instruction *completion_inst = nullptr;
  MPIRequestState request_state = MPIRequestState::Unbound;
  bool matched_message = false;

  WindowID window = nullptr;
  GroupID group = nullptr;
  int target_rank = -1;
  int target_rank_min = -1;
  int target_rank_max = -1;
  int64_t target_disp = -1;
  int64_t byte_length = -1;
  bool rma_lock_all = false;
  bool rma_local_completion_only = false;

  const llvm::Value *datatype = nullptr;
  int64_t datatype_size = -1;
  bool is_derived_datatype = false;

  RMAEpochKind rma_epoch_kind = RMAEpochKind::None;
  concurrency::ProofStrength synchronization_proof =
      concurrency::ProofStrength::Unknown;

  MPIOperation() = default;
  MPIOperation(const llvm::Instruction *i, MPIOpKind k, ThreadAPI::TD_TYPE t)
      : inst(i), kind(k), td_type(t), function(i ? i->getFunction() : nullptr) {}
};

} // namespace mpi

#endif // MPI_OPERATION_H
