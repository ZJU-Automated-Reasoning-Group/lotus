#include "Analysis/Concurrency/MPI/MPISemantics.h"

#include "Analysis/Concurrency/MPI/MPISymbol.h"

#include <limits>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace mpi {

namespace {

using TD = ThreadAPI::TD_TYPE;

MPISendMode classifySendMode(llvm::StringRef canonical_name) {
  if (canonical_name.contains("Ssend")) {
    return MPISendMode::Synchronous;
  }
  if (canonical_name.contains("Bsend")) {
    return MPISendMode::Buffered;
  }
  if (canonical_name.contains("Rsend")) {
    return MPISendMode::Ready;
  }
  if (canonical_name.startswith("MPI_Send") ||
      canonical_name.startswith("MPI_Isend")) {
    return MPISendMode::Standard;
  }
  return MPISendMode::Unknown;
}

MPIBlockingMode classifyBlockingMode(TD type, llvm::StringRef canonical_name) {
  switch (type) {
  case TD::TD_MPI_ISEND:
  case TD::TD_MPI_IRECV:
  case TD::TD_MPI_IPROBE:
  case TD::TD_MPI_IMPROBE:
  case TD::TD_MPI_IMRECV:
    return MPIBlockingMode::NonBlocking;
  case TD::TD_MPI_MPROBE:
  case TD::TD_MPI_MRECV:
    return MPIBlockingMode::Blocking;
  case TD::TD_MPI_WAIT:
  case TD::TD_MPI_WAITALL:
  case TD::TD_MPI_WAITANY:
  case TD::TD_MPI_WAITSOME:
  case TD::TD_MPI_TEST:
  case TD::TD_MPI_TESTALL:
  case TD::TD_MPI_TESTANY:
  case TD::TD_MPI_TESTSOME:
    return MPIBlockingMode::Completion;
  case TD::TD_MPI_WIN_FLUSH:
    return canonical_name.contains("local")
               ? MPIBlockingMode::LocalCompletion
               : MPIBlockingMode::Completion;
  case TD::TD_MPI_BARRIER:
  case TD::TD_MPI_BCAST:
  case TD::TD_MPI_SCATTER:
  case TD::TD_MPI_GATHER:
  case TD::TD_MPI_ALLGATHER:
  case TD::TD_MPI_ALLTOALL:
  case TD::TD_MPI_REDUCE:
  case TD::TD_MPI_ALLREDUCE:
  case TD::TD_MPI_REDUCE_SCATTER:
  case TD::TD_MPI_SCAN:
    return canonical_name.startswith("MPI_I")
               ? MPIBlockingMode::NonBlocking
               : MPIBlockingMode::Blocking;
  default:
    break;
  }

  return MPIBlockingMode::Blocking;
}

MPIRequestArity classifyRequestArity(TD type, llvm::StringRef canonical_name) {
  switch (type) {
  case TD::TD_MPI_ISEND:
  case TD::TD_MPI_IRECV:
  case TD::TD_MPI_BARRIER:
  case TD::TD_MPI_BCAST:
  case TD::TD_MPI_SCATTER:
  case TD::TD_MPI_GATHER:
  case TD::TD_MPI_ALLGATHER:
  case TD::TD_MPI_ALLTOALL:
  case TD::TD_MPI_REDUCE:
  case TD::TD_MPI_ALLREDUCE:
  case TD::TD_MPI_REDUCE_SCATTER:
  case TD::TD_MPI_SCAN:
    return canonical_name.startswith("MPI_I") ? MPIRequestArity::Single
                                              : MPIRequestArity::None;
  case TD::TD_MPI_WAIT:
  case TD::TD_MPI_TEST:
  case TD::TD_MPI_REQUEST_FREE:
  case TD::TD_MPI_CANCEL:
  case TD::TD_MPI_IMRECV:
    return MPIRequestArity::Single;
  case TD::TD_MPI_REQUEST_START:
    return canonical_name.equals("MPI_Startall") ? MPIRequestArity::Array
                                                 : MPIRequestArity::Single;
  case TD::TD_MPI_WAITALL:
  case TD::TD_MPI_WAITANY:
  case TD::TD_MPI_WAITSOME:
  case TD::TD_MPI_TESTALL:
  case TD::TD_MPI_TESTANY:
  case TD::TD_MPI_TESTSOME:
    return MPIRequestArity::Array;
  default:
    return MPIRequestArity::None;
  }
}

MPICollectiveVariant classifyCollectiveVariant(TD type,
                                              llvm::StringRef semantic_tag) {
  switch (type) {
  case TD::TD_MPI_BARRIER:
    return MPICollectiveVariant::Barrier;
  case TD::TD_MPI_BCAST:
    return semantic_tag.startswith("intercomm-")
               ? MPICollectiveVariant::IntercommBcast
               : MPICollectiveVariant::Bcast;
  case TD::TD_MPI_SCATTER:
    return semantic_tag.contains("scatterv") ? MPICollectiveVariant::Scatterv
                                             : MPICollectiveVariant::Scatter;
  case TD::TD_MPI_GATHER:
    return semantic_tag.contains("gatherv") ? MPICollectiveVariant::Gatherv
                                            : MPICollectiveVariant::Gather;
  case TD::TD_MPI_ALLGATHER:
    return semantic_tag.contains("gatherv")
               ? MPICollectiveVariant::Allgatherv
               : MPICollectiveVariant::Allgather;
  case TD::TD_MPI_ALLTOALL:
    if (semantic_tag.contains("alltoallw")) {
      return MPICollectiveVariant::Alltoallw;
    }
    return semantic_tag.contains("alltoallv")
               ? MPICollectiveVariant::Alltoallv
               : MPICollectiveVariant::Alltoall;
  case TD::TD_MPI_REDUCE:
    return MPICollectiveVariant::Reduce;
  case TD::TD_MPI_ALLREDUCE:
    return MPICollectiveVariant::Allreduce;
  case TD::TD_MPI_REDUCE_SCATTER:
    return semantic_tag.contains("block")
               ? MPICollectiveVariant::ReduceScatterBlock
               : MPICollectiveVariant::ReduceScatter;
  case TD::TD_MPI_SCAN:
    return semantic_tag.contains("exscan") ? MPICollectiveVariant::Exscan
                                           : MPICollectiveVariant::Scan;
  default:
    break;
  }

  if (semantic_tag.startswith("neighbor-") || semantic_tag.startswith("ineighbor-")) {
    if (semantic_tag.contains("allgatherv")) {
      return MPICollectiveVariant::NeighborAllgatherv;
    }
    if (semantic_tag.contains("allgather")) {
      return MPICollectiveVariant::NeighborAllgather;
    }
    if (semantic_tag.contains("alltoallw")) {
      return MPICollectiveVariant::NeighborAlltoallw;
    }
    if (semantic_tag.contains("alltoallv")) {
      return MPICollectiveVariant::NeighborAlltoallv;
    }
    if (semantic_tag.contains("alltoall")) {
      return MPICollectiveVariant::NeighborAlltoall;
    }
  }

  return MPICollectiveVariant::Unknown;
}

MPICollectiveShape classifyCollectiveShape(MPICollectiveVariant variant,
                                           llvm::StringRef semantic_tag) {
  if (semantic_tag.startswith("intercomm-")) {
    return MPICollectiveShape::Intercommunicator;
  }
  if (semantic_tag.startswith("neighbor-") || semantic_tag.startswith("ineighbor-")) {
    return MPICollectiveShape::Neighbor;
  }

  switch (variant) {
  case MPICollectiveVariant::Barrier:
    return MPICollectiveShape::Barrier;
  case MPICollectiveVariant::Bcast:
  case MPICollectiveVariant::Gather:
  case MPICollectiveVariant::Gatherv:
  case MPICollectiveVariant::Scatter:
  case MPICollectiveVariant::Scatterv:
  case MPICollectiveVariant::IntercommBcast:
    return MPICollectiveShape::Rooted;
  case MPICollectiveVariant::Allgather:
  case MPICollectiveVariant::Allgatherv:
  case MPICollectiveVariant::Alltoall:
  case MPICollectiveVariant::Alltoallv:
  case MPICollectiveVariant::Alltoallw:
    return MPICollectiveShape::AllToAll;
  case MPICollectiveVariant::Reduce:
  case MPICollectiveVariant::Allreduce:
  case MPICollectiveVariant::ReduceScatter:
  case MPICollectiveVariant::ReduceScatterBlock:
    return MPICollectiveShape::Reduction;
  case MPICollectiveVariant::Scan:
  case MPICollectiveVariant::Exscan:
    return MPICollectiveShape::Scan;
  case MPICollectiveVariant::NeighborAllgather:
  case MPICollectiveVariant::NeighborAllgatherv:
  case MPICollectiveVariant::NeighborAlltoall:
  case MPICollectiveVariant::NeighborAlltoallv:
  case MPICollectiveVariant::NeighborAlltoallw:
    return MPICollectiveShape::Neighbor;
  case MPICollectiveVariant::Unknown:
    return MPICollectiveShape::Unknown;
  }
  return MPICollectiveShape::Unknown;
}

MPIRMAAccessKind classifyRMAAccessKind(llvm::StringRef canonical_name) {
  if (canonical_name.equals("MPI_Put") || canonical_name.equals("MPI_Rput")) {
    return MPIRMAAccessKind::Put;
  }
  if (canonical_name.equals("MPI_Get") || canonical_name.equals("MPI_Rget")) {
    return MPIRMAAccessKind::Get;
  }
  if (canonical_name.equals("MPI_Accumulate") ||
      canonical_name.equals("MPI_Raccumulate")) {
    return MPIRMAAccessKind::Accumulate;
  }
  if (canonical_name.equals("MPI_Get_accumulate") ||
      canonical_name.equals("MPI_Rget_accumulate") ||
      canonical_name.equals("MPI_Fetch_and_op") ||
      canonical_name.equals("MPI_Compare_and_swap")) {
    return MPIRMAAccessKind::Atomic;
  }
  return MPIRMAAccessKind::None;
}

MPIRMASyncKind classifyRMASyncKind(llvm::StringRef canonical_name) {
  if (canonical_name.equals("MPI_Win_fence")) {
    return MPIRMASyncKind::Fence;
  }
  if (canonical_name.equals("MPI_Win_lock")) {
    return MPIRMASyncKind::Lock;
  }
  if (canonical_name.equals("MPI_Win_lock_all")) {
    return MPIRMASyncKind::LockAll;
  }
  if (canonical_name.equals("MPI_Win_unlock")) {
    return MPIRMASyncKind::Unlock;
  }
  if (canonical_name.equals("MPI_Win_unlock_all")) {
    return MPIRMASyncKind::UnlockAll;
  }
  if (canonical_name.equals("MPI_Win_flush")) {
    return MPIRMASyncKind::Flush;
  }
  if (canonical_name.equals("MPI_Win_flush_all")) {
    return MPIRMASyncKind::FlushAll;
  }
  if (canonical_name.equals("MPI_Win_flush_local")) {
    return MPIRMASyncKind::FlushLocal;
  }
  if (canonical_name.equals("MPI_Win_flush_local_all")) {
    return MPIRMASyncKind::FlushLocalAll;
  }
  if (canonical_name.equals("MPI_Win_sync")) {
    return MPIRMASyncKind::Sync;
  }
  if (canonical_name.equals("MPI_Win_post")) {
    return MPIRMASyncKind::PSCWPost;
  }
  if (canonical_name.equals("MPI_Win_start")) {
    return MPIRMASyncKind::PSCWStart;
  }
  if (canonical_name.equals("MPI_Win_complete")) {
    return MPIRMASyncKind::PSCWComplete;
  }
  if (canonical_name.equals("MPI_Win_wait")) {
    return MPIRMASyncKind::PSCWWait;
  }
  if (canonical_name.equals("MPI_Win_test")) {
    return MPIRMASyncKind::PSCWTest;
  }
  return MPIRMASyncKind::None;
}

constexpr MPISemanticDescriptor
makeDesc(TD type, MPIOpKind kind, MPISemanticFamily family,
         int communicator_arg = -1, int request_arg = -1, int count_arg = -1,
         int datatype_arg = -1, int peer_rank_arg = -1, int tag_arg = -1,
         bool peer_rank_is_dest = false) {
  MPISemanticDescriptor descriptor;
  descriptor.type = type;
  descriptor.kind = kind;
  descriptor.family = family;
  descriptor.communicator_arg = communicator_arg;
  descriptor.request_arg = request_arg;
  descriptor.count_arg = count_arg;
  descriptor.datatype_arg = datatype_arg;
  descriptor.peer_rank_arg = peer_rank_arg;
  descriptor.tag_arg = tag_arg;
  descriptor.peer_rank_is_dest = peer_rank_is_dest;
  return descriptor;
}

constexpr int kArgAbsent = std::numeric_limits<int>::min();
constexpr int kKeepDefaultArg = std::numeric_limits<int>::min() + 1;

struct MPISymbolSemanticOverride {
  llvm::StringRef semantic_tag;
  llvm::StringRef canonical_name;
  int communicator_arg = kKeepDefaultArg;
  int request_arg = kKeepDefaultArg;
  int result_handle_arg = kKeepDefaultArg;
  MPIOpKind kind_override = MPIOpKind::UNKNOWN;
  MPICommunicatorSemanticKind communicator_semantic =
      MPICommunicatorSemanticKind::None;
  bool request_lifecycle_issue_nonblocking = false;
};

constexpr MPISemanticDescriptor makeSendRecvDesc(TD type) {
  MPISemanticDescriptor descriptor =
      makeDesc(type, MPIOpKind::UNKNOWN, MPISemanticFamily::PointToPoint);
  descriptor.split_into_sendrecv = true;
  return descriptor;
}

constexpr MPISemanticDescriptor makeMatchedRecvDesc(TD type,
                                                    MPIOpKind kind,
                                                    int request_arg = kArgAbsent) {
  MPISemanticDescriptor descriptor =
      makeDesc(type, kind, MPISemanticFamily::PointToPoint, kArgAbsent,
               request_arg, 1, 2, kArgAbsent, kArgAbsent, false);
  return descriptor;
}

constexpr MPISemanticDescriptor makeCollectiveDesc(TD type,
                                                   bool barrier_family) {
  MPISemanticDescriptor descriptor =
      makeDesc(type,
               barrier_family ? MPIOpKind::BARRIER_BLOCKING
                              : MPIOpKind::COLLECTIVE_BLOCKING,
               MPISemanticFamily::Collective, -1);
  descriptor.trait_driven_barrier_kind = barrier_family;
  descriptor.trait_driven_collective_kind = !barrier_family;
  descriptor.collective_nonblocking_comm_arg = -2;
  descriptor.collective_nonblocking_request_arg = -1;
  return descriptor;
}

constexpr MPISemanticDescriptor makeRMADataDesc(TD type) {
  MPISemanticDescriptor descriptor =
      makeDesc(type, MPIOpKind::RMA_DATA, MPISemanticFamily::RMAData);
  descriptor.count_arg = 1;
  descriptor.datatype_arg = 2;
  descriptor.target_rank_arg = 3;
  descriptor.target_disp_arg = 4;
  descriptor.window_arg = 7;
  return descriptor;
}

constexpr MPISemanticDescriptor makeRMAWindowCreateDesc() {
  MPISemanticDescriptor descriptor =
      makeDesc(TD::TD_MPI_WIN_CREATE, MPIOpKind::RMA_WINDOW,
               MPISemanticFamily::RMAWindow);
  descriptor.result_handle_arg = -1;
  return descriptor;
}

constexpr MPISemanticDescriptor makeRMAWindowFreeDesc() {
  MPISemanticDescriptor descriptor = makeDesc(
      TD::TD_MPI_WIN_FREE, MPIOpKind::RMA_WINDOW, MPISemanticFamily::RMAWindow);
  descriptor.window_arg = 0;
  return descriptor;
}

constexpr MPISemanticDescriptor makePSCWSyncDesc(TD type) {
  MPISemanticDescriptor descriptor =
      makeDesc(type, MPIOpKind::RMA_SYNC, MPISemanticFamily::RMASync);
  descriptor.group_arg = 0;
  descriptor.window_arg = 2;
  return descriptor;
}

const MPISemanticDescriptor kDescriptors[] = {
    makeDesc(TD::TD_MPI_SESSION_INIT, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_FINALIZE, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_GET_INFO, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_GET_NUM_ERRCODES, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_GET_ERRHANDLER, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_SESSION_SET_ERRHANDLER, MPIOpKind::SESSION,
             MPISemanticFamily::Session),
    makeDesc(TD::TD_MPI_INIT, MPIOpKind::INIT, MPISemanticFamily::Lifecycle),
    makeDesc(TD::TD_MPI_FINALIZE, MPIOpKind::FINALIZE,
             MPISemanticFamily::Lifecycle),
    makeDesc(TD::TD_MPI_SEND, MPIOpKind::SEND_BLOCKING,
             MPISemanticFamily::PointToPoint, 5, -1, 1, 2, 3, 4, true),
    makeDesc(TD::TD_MPI_RECV, MPIOpKind::RECV_BLOCKING,
             MPISemanticFamily::PointToPoint, 5, -1, 1, 2, 3, 4, false),
    makeSendRecvDesc(TD::TD_MPI_SENDRECV),
    makeDesc(TD::TD_MPI_PROBE, MPIOpKind::PROBE_BLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_ISEND, MPIOpKind::SEND_NONBLOCKING,
             MPISemanticFamily::PointToPoint, 5, 6, 1, 2, 3, 4, true),
    makeDesc(TD::TD_MPI_IRECV, MPIOpKind::RECV_NONBLOCKING,
             MPISemanticFamily::PointToPoint, 5, 6, 1, 2, 3, 4, false),
    makeDesc(TD::TD_MPI_IPROBE, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_WAIT, MPIOpKind::WAIT, MPISemanticFamily::Request, -1,
             0),
    makeDesc(TD::TD_MPI_WAITALL, MPIOpKind::WAIT, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_WAITANY, MPIOpKind::WAIT, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_WAITSOME, MPIOpKind::WAIT, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_TEST, MPIOpKind::TEST, MPISemanticFamily::Request, -1,
             0),
    makeDesc(TD::TD_MPI_TESTALL, MPIOpKind::TEST, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_TESTANY, MPIOpKind::TEST, MPISemanticFamily::Request,
             -1, 1),
    makeDesc(TD::TD_MPI_TESTSOME, MPIOpKind::TEST, MPISemanticFamily::Request,
             -1, 1),
    makeCollectiveDesc(TD::TD_MPI_BARRIER, true),
    makeCollectiveDesc(TD::TD_MPI_BCAST, false),
    makeCollectiveDesc(TD::TD_MPI_SCATTER, false),
    makeCollectiveDesc(TD::TD_MPI_GATHER, false),
    makeCollectiveDesc(TD::TD_MPI_ALLGATHER, false),
    makeCollectiveDesc(TD::TD_MPI_ALLTOALL, false),
    makeCollectiveDesc(TD::TD_MPI_REDUCE, false),
    makeCollectiveDesc(TD::TD_MPI_ALLREDUCE, false),
    makeCollectiveDesc(TD::TD_MPI_REDUCE_SCATTER, false),
    makeCollectiveDesc(TD::TD_MPI_SCAN, false),
    makeRMAWindowCreateDesc(),
    makeRMAWindowFreeDesc(),
    makeRMADataDesc(TD::TD_MPI_PUT),
    makeRMADataDesc(TD::TD_MPI_GET),
    makeRMADataDesc(TD::TD_MPI_ACCUMULATE),
    makeDesc(TD::TD_MPI_WIN_FENCE, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_LOCK, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_UNLOCK, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_FLUSH, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_SYNC, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makePSCWSyncDesc(TD::TD_MPI_WIN_POST),
    makePSCWSyncDesc(TD::TD_MPI_WIN_START),
    makeDesc(TD::TD_MPI_WIN_COMPLETE, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_WAIT, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    makeDesc(TD::TD_MPI_WIN_TEST, MPIOpKind::RMA_SYNC,
             MPISemanticFamily::RMASync),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_COMM_DUP, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = 1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::Duplicate;
      return descriptor;
    }(),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_COMM_SPLIT, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::Split;
      return descriptor;
    }(),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_COMM_CREATE, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::Create;
      return descriptor;
    }(),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_COMM_FREE, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::Free;
      return descriptor;
    }(),
    makeDesc(TD::TD_MPI_PERSISTENT_SEND_INIT, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, 5, 6, 1, 2, 3, 4, true),
    makeDesc(TD::TD_MPI_PERSISTENT_RECV_INIT, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, 5, 6, 1, 2, 3, 4, false),
    makeDesc(TD::TD_MPI_REQUEST_START, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, -1, 1),
    makeDesc(TD::TD_MPI_REQUEST_FREE, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, -1, 0),
    makeDesc(TD::TD_MPI_CANCEL, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request, -1, 0),
    makeDesc(TD::TD_MPI_GET_COUNT, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_GET_ELEMENTS, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_GET_ELEMENTS_X, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_STATUS_SIZE, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_STATUS_SET_ELEMENTS, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_STATUS_SET_ELEMENTS_X, MPIOpKind::REQUEST_MANAGEMENT,
             MPISemanticFamily::Request),
    makeDesc(TD::TD_MPI_MPROBE, MPIOpKind::PROBE_BLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeDesc(TD::TD_MPI_IMPROBE, MPIOpKind::PROBE_NONBLOCKING,
             MPISemanticFamily::Probe, 2, -1, -1, -1, 0, 1, false),
    makeMatchedRecvDesc(TD::TD_MPI_IMRECV, MPIOpKind::RECV_NONBLOCKING, 4),
    makeMatchedRecvDesc(TD::TD_MPI_MRECV, MPIOpKind::RECV_BLOCKING),
    makeDesc(TD::TD_MPI_TYPE_CONTIGUOUS, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_VECTOR, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_HVECTOR, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_INDEXED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_HINDEXED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_STRUCT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_DLPACK, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_SUBARRAY, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_DARRAY, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_RESIZED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_HINDEXED, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_CREATE_HVECTOR, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_GET_EXTENT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_GET_TRUE_EXTENT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_SIZE, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    makeDesc(TD::TD_MPI_TYPE_COMMIT, MPIOpKind::DATATYPE_CREATE,
             MPISemanticFamily::Datatype),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_CART_CREATE, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::TopologyCreate;
      return descriptor;
    }(),
    makeDesc(TD::TD_MPI_CART_DIMS_CREATE, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_GET, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_SHIFT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_COORDS, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_CART_RANK, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_CART_SUB, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::TopologyCreate;
      return descriptor;
    }(),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_DIST_GRAPH_CREATE, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::TopologyCreate;
      return descriptor;
    }(),
    [] {
      MPISemanticDescriptor descriptor = makeDesc(
          TD::TD_MPI_DIST_GRAPH_CREATE_ADJACENT, MPIOpKind::COMM_MANAGEMENT,
          MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::TopologyCreate;
      return descriptor;
    }(),
    makeDesc(TD::TD_MPI_DIST_GRAPH_NEIGHBORS, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    [] {
      MPISemanticDescriptor descriptor =
          makeDesc(TD::TD_MPI_GRAPH_CREATE, MPIOpKind::COMM_MANAGEMENT,
                   MPISemanticFamily::Communicator, 0);
      descriptor.result_handle_arg = -1;
      descriptor.communicator_semantic = MPICommunicatorSemanticKind::TopologyCreate;
      return descriptor;
    }(),
    makeDesc(TD::TD_MPI_GRAPH_GET, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_NEIGHBORS, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_NEIGHBORS_COUNT, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_DIMS_GET, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
    makeDesc(TD::TD_MPI_GRAPH_MAP, MPIOpKind::COMM_MANAGEMENT,
             MPISemanticFamily::Communicator),
};

constexpr MPISymbolSemanticOverride kSymbolOverrides[] = {
    {"win-create", "MPI_Win_create", -2, kKeepDefaultArg, -1,
     MPIOpKind::UNKNOWN, MPICommunicatorSemanticKind::None, false},
    {"win-create-dynamic", "MPI_Win_create_dynamic", -2, kKeepDefaultArg, -1,
     MPIOpKind::UNKNOWN, MPICommunicatorSemanticKind::None, false},
    {"win-allocate", "MPI_Win_allocate", -2, kKeepDefaultArg, -1,
     MPIOpKind::UNKNOWN, MPICommunicatorSemanticKind::None, false},
    {"win-allocate-shared", "MPI_Win_allocate_shared", -2, kKeepDefaultArg, -1,
     MPIOpKind::UNKNOWN, MPICommunicatorSemanticKind::None, false},
    {"comm-dup-with-info", "MPI_Comm_dup_with_info", 0, kKeepDefaultArg, -1,
     MPIOpKind::COMM_MANAGEMENT, MPICommunicatorSemanticKind::Duplicate, false},
    {"comm-idup", "MPI_Comm_idup", 0, 2, 1, MPIOpKind::COMM_MANAGEMENT,
     MPICommunicatorSemanticKind::Duplicate, true},
    {"intercomm-create", "MPI_Intercomm_create", 0, kKeepDefaultArg, -1,
     MPIOpKind::INTERCOMM_CREATION,
     MPICommunicatorSemanticKind::IntercommunicatorCreate, false},
    {"intercomm-create-from-groups", "MPI_Intercomm_create_from_groups", 0,
     kKeepDefaultArg, -1, MPIOpKind::INTERCOMM_CREATION,
     MPICommunicatorSemanticKind::IntercommunicatorCreate, false},
    {"intercomm-merge", "MPI_Intercomm_merge", 0, kKeepDefaultArg, -1,
     MPIOpKind::INTERCOMM_CREATION,
     MPICommunicatorSemanticKind::IntercommunicatorCreate, false},
};

const MPISymbolSemanticOverride *
lookupMPISymbolOverride(llvm::StringRef canonical_name,
                        llvm::StringRef semantic_tag) {
  for (const MPISymbolSemanticOverride &entry : kSymbolOverrides) {
    if ((!entry.semantic_tag.empty() && entry.semantic_tag == semantic_tag) ||
        (!entry.canonical_name.empty() && entry.canonical_name == canonical_name)) {
      return &entry;
    }
  }
  return nullptr;
}

void applySignedIndexOverride(int &slot, int override_value) {
  if (override_value != kKeepDefaultArg) {
    slot = override_value;
  }
}

} // namespace

const MPISemanticDescriptor *lookupMPISemantic(ThreadAPI::TD_TYPE type) {
  for (const MPISemanticDescriptor &descriptor : kDescriptors) {
    if (descriptor.type == type) {
      return &descriptor;
    }
  }
  return nullptr;
}

MPIEffect buildMPIEffect(const llvm::Instruction *inst, ThreadAPI *api) {
  MPIEffect effect;
  if (!inst || !api) {
    return effect;
  }

  const auto *cb = llvm::dyn_cast<llvm::CallBase>(inst);
  if (!cb) {
    return effect;
  }

  const llvm::Function *callee = api->getCallee(inst);
  if (!callee) {
    return effect;
  }

  MPISymbolNormalization normalization = normalizeMPISymbol(callee->getName());
  effect.confidence = normalization.confidence;
  effect.semantic_tag = api->getSemanticTag(callee);
  const llvm::StringRef canonical_name = normalization.canonical_name;
  effect.type = api->getType(callee);
  const MPISemanticDescriptor *base_descriptor = lookupMPISemantic(effect.type);
  if (!base_descriptor) {
    return effect;
  }
  effect.descriptor = *base_descriptor;
  effect.has_descriptor = true;

  if (const MPISymbolSemanticOverride *override =
          lookupMPISymbolOverride(canonical_name, effect.semantic_tag)) {
    applySignedIndexOverride(effect.descriptor.communicator_arg,
                             override->communicator_arg);
    applySignedIndexOverride(effect.descriptor.request_arg, override->request_arg);
    applySignedIndexOverride(effect.descriptor.result_handle_arg,
                             override->result_handle_arg);
    if (override->kind_override != MPIOpKind::UNKNOWN) {
      effect.descriptor.kind = override->kind_override;
    }
    if (override->communicator_semantic != MPICommunicatorSemanticKind::None) {
      effect.descriptor.communicator_semantic = override->communicator_semantic;
    }
    effect.descriptor.request_lifecycle_issue_nonblocking =
        override->request_lifecycle_issue_nonblocking;
  }

  effect.family = effect.descriptor.family;
  effect.kind = effect.descriptor.kind;
  if (effect.descriptor.trait_driven_barrier_kind) {
    effect.kind = canonical_name.startswith("MPI_I")
                      ? MPIOpKind::BARRIER_NONBLOCKING
                      : MPIOpKind::BARRIER_BLOCKING;
  } else if (effect.descriptor.trait_driven_collective_kind) {
    effect.kind = canonical_name.startswith("MPI_I")
                      ? MPIOpKind::COLLECTIVE_NONBLOCKING
                      : MPIOpKind::COLLECTIVE_BLOCKING;
  }
  switch (effect.family) {
  case MPISemanticFamily::Lifecycle:
    effect.effect_kind = MPIEffectKind::Lifecycle;
    break;
  case MPISemanticFamily::Session:
    effect.effect_kind = MPIEffectKind::Session;
    break;
  case MPISemanticFamily::PointToPoint:
    effect.effect_kind = MPIEffectKind::PointToPoint;
    break;
  case MPISemanticFamily::Probe:
    effect.effect_kind = MPIEffectKind::Probe;
    break;
  case MPISemanticFamily::Request:
    effect.effect_kind = MPIEffectKind::Request;
    break;
  case MPISemanticFamily::Collective:
    effect.effect_kind = MPIEffectKind::Collective;
    break;
  case MPISemanticFamily::Communicator:
    effect.effect_kind = MPIEffectKind::Communicator;
    break;
  case MPISemanticFamily::RMAWindow:
    effect.effect_kind = MPIEffectKind::RMAWindow;
    break;
  case MPISemanticFamily::RMAData:
    effect.effect_kind = MPIEffectKind::RMAData;
    break;
  case MPISemanticFamily::RMASync:
    effect.effect_kind = MPIEffectKind::RMASync;
    break;
  case MPISemanticFamily::Datatype:
    effect.effect_kind = MPIEffectKind::Datatype;
    break;
  case MPISemanticFamily::Unknown:
    effect.effect_kind = MPIEffectKind::Unknown;
    break;
  }

  effect.send_mode = classifySendMode(canonical_name);
  effect.blocking_mode = classifyBlockingMode(effect.type, canonical_name);
  effect.request_arity = classifyRequestArity(effect.type, canonical_name);
  effect.collective_variant =
      classifyCollectiveVariant(effect.type, effect.semantic_tag);
  effect.collective_shape =
      classifyCollectiveShape(effect.collective_variant, effect.semantic_tag);
  effect.rma_access_kind = classifyRMAAccessKind(canonical_name);
  effect.rma_sync_kind = classifyRMASyncKind(canonical_name);
  effect.rma_local_completion_only =
      effect.rma_sync_kind == MPIRMASyncKind::FlushLocal ||
      effect.rma_sync_kind == MPIRMASyncKind::FlushLocalAll;
  if (effect.descriptor.request_lifecycle_issue_nonblocking &&
      effect.descriptor.request_arg != -1) {
    effect.request_arity = MPIRequestArity::Single;
    effect.blocking_mode = MPIBlockingMode::NonBlocking;
  }

  return effect;
}

} // namespace mpi
