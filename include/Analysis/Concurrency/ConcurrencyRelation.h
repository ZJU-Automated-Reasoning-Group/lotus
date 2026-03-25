#pragma once

#include <string>

namespace concurrency {

enum class ProofStrength {
  Must,
  May,
  Unknown
};

enum class RelationKind {
  MustHappenBefore,
  SelectiveHappenBefore,
  MayHappenBefore,
  MutuallyExclusive,
  MatchedCommunication,
  MPICollectiveParticipation,
  MPICollectiveLocalCompletion,
  MPIGlobalBarrier,
  MPIRequestCompletion,
  MPICommunicatorProvenance,
  SameSynchronizationEpoch,
  SameCollectiveFrontier,
  SameProtocolSlot,
  DisjointParticipants,
  LocalOnlySynchronizationCompletion,
  UnknownDueToModelGap
};

struct Relation {
  RelationKind kind = RelationKind::UnknownDueToModelGap;
  ProofStrength proof = ProofStrength::Unknown;
  std::string reason;
};

inline int relationPriority(RelationKind kind) {
  switch (kind) {
  case RelationKind::MustHappenBefore:
    return 6;
  case RelationKind::SelectiveHappenBefore:
    return 5;
  case RelationKind::MutuallyExclusive:
    return 4;
  case RelationKind::MatchedCommunication:
    return 4;
  case RelationKind::MPICollectiveParticipation:
    return 4;
  case RelationKind::MPICollectiveLocalCompletion:
    return 4;
  case RelationKind::MPIGlobalBarrier:
    return 4;
  case RelationKind::MPIRequestCompletion:
    return 4;
  case RelationKind::MPICommunicatorProvenance:
    return 4;
  case RelationKind::SameSynchronizationEpoch:
    return 3;
  case RelationKind::SameCollectiveFrontier:
    return 3;
  case RelationKind::SameProtocolSlot:
    return 3;
  case RelationKind::DisjointParticipants:
    return 3;
  case RelationKind::LocalOnlySynchronizationCompletion:
    return 3;
  case RelationKind::MayHappenBefore:
    return 2;
  case RelationKind::UnknownDueToModelGap:
    return 1;
  }
  return 0;
}

} // namespace concurrency
