#pragma once

#include "Analysis/Concurrency/MPI/MPINormalization.h"
#include "Analysis/Concurrency/MPI/MPIOperation.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <string>

namespace mpi {

enum class MPIEffectKind {
  Lifecycle,
  Session,
  PointToPoint,
  Probe,
  Request,
  Collective,
  Communicator,
  RMAWindow,
  RMAData,
  RMASync,
  Datatype,
  Unknown
};

enum class MPISemanticFamily {
  Lifecycle,
  Session,
  PointToPoint,
  Probe,
  Request,
  Collective,
  Communicator,
  RMAWindow,
  RMAData,
  RMASync,
  Datatype,
  Unknown
};

enum class MPICommunicatorSemanticKind {
  None,
  Duplicate,
  Split,
  Create,
  IntercommunicatorCreate,
  TopologyCreate,
  Free
};

struct MPISemanticDescriptor {
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  MPIOpKind kind = MPIOpKind::UNKNOWN;
  MPISemanticFamily family = MPISemanticFamily::Unknown;
  bool trait_driven_barrier_kind = false;
  bool trait_driven_collective_kind = false;
  bool split_into_sendrecv = false;
  bool request_lifecycle_issue_nonblocking = false;
  MPICommunicatorSemanticKind communicator_semantic =
      MPICommunicatorSemanticKind::None;

  // Signed argument indices: non-negative are absolute, negative are from end
  // (-1 = last argument).
  int communicator_arg = -1;
  int request_arg = -1;
  int result_handle_arg = -1;
  int count_arg = -1;
  int datatype_arg = -1;
  int peer_rank_arg = -1;
  int tag_arg = -1;
  int window_arg = -1;
  int group_arg = -1;
  int target_rank_arg = -1;
  int target_disp_arg = -1;
  int recv_count_arg = -1;
  int recv_datatype_arg = -1;
  int root_arg = -1;
  int reduction_op_arg = -1;
  bool peer_rank_is_dest = false;

  int collective_nonblocking_comm_arg = -1;
  int collective_nonblocking_request_arg = -1;
};

struct MPIEffect {
  MPIEffectKind effect_kind = MPIEffectKind::Unknown;
  ThreadAPI::TD_TYPE type = ThreadAPI::TD_DUMMY;
  MPIOpKind kind = MPIOpKind::UNKNOWN;
  MPISemanticFamily family = MPISemanticFamily::Unknown;
  NormalizationConfidence confidence =
      NormalizationConfidence::UnknownVendorInternal;
  std::string semantic_tag;
  MPISemanticDescriptor descriptor;
  bool has_descriptor = false;
  MPISendMode send_mode = MPISendMode::Unknown;
  MPIBlockingMode blocking_mode = MPIBlockingMode::Unknown;
  MPIRequestArity request_arity = MPIRequestArity::None;
  MPICollectiveVariant collective_variant = MPICollectiveVariant::Unknown;
  MPICollectiveShape collective_shape = MPICollectiveShape::Unknown;
  MPIRMAAccessKind rma_access_kind = MPIRMAAccessKind::None;
  MPIRMASyncKind rma_sync_kind = MPIRMASyncKind::None;
  bool rma_local_completion_only = false;
};

const MPISemanticDescriptor *lookupMPISemantic(ThreadAPI::TD_TYPE type);
MPIEffect buildMPIEffect(const llvm::Instruction *inst, ThreadAPI *api);

} // namespace mpi
