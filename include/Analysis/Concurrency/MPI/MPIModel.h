/**
 * @file MPIModel.h
 * @brief MPI (Message Passing Interface) Language Model
 *
 * This file provides pattern matching and recognition for MPI library
 * functions. Supports MPI-1, MPI-2 (one-sided), and MPI-3 (non-blocking
 * collectives).
 *
 * @author Lotus Analysis Framework
 * @date 2026
 * @ingroup Concurrency
 */

#pragma once

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>

namespace MPIModel {

// ============================================================================
// Process Management
// ============================================================================

inline bool isInit(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Init") || funcName.equals("MPI_Init_thread");
}

inline bool isFinalize(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Finalize");
}

inline bool isCommSize(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Comm_size");
}

inline bool isCommRank(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Comm_rank");
}

// ============================================================================
// Point-to-Point Communication (Blocking)
// ============================================================================

inline bool isSend(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Send") || funcName.equals("MPI_Ssend") ||
         funcName.equals("MPI_Bsend") || funcName.equals("MPI_Rsend");
}

inline bool isRecv(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Recv");
}

inline bool isSendrecv(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Sendrecv") ||
         funcName.equals("MPI_Sendrecv_replace");
}

inline bool isProbe(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Probe");
}

// ============================================================================
// Point-to-Point Communication (Non-blocking)
// ============================================================================

inline bool isIsend(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Isend") || funcName.equals("MPI_Issend") ||
         funcName.equals("MPI_Ibsend") || funcName.equals("MPI_Irsend");
}

inline bool isIrecv(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Irecv");
}

inline bool isIprobe(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Iprobe");
}

// ============================================================================
// Synchronization and Completion
// ============================================================================

inline bool isWait(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Wait");
}

inline bool isWaitall(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Waitall");
}

inline bool isWaitany(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Waitany");
}

inline bool isWaitsome(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Waitsome");
}

inline bool isTest(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Test");
}

inline bool isTestall(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Testall");
}

inline bool isTestany(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Testany");
}

inline bool isTestsome(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Testsome");
}

// ============================================================================
// Collective Communication (Synchronizing)
// ============================================================================

inline bool isBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Barrier") || funcName.equals("MPI_Ibarrier");
}

inline bool isBlockingBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Barrier");
}

inline bool isNonBlockingBarrier(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Ibarrier");
}

inline bool isBcast(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Bcast") || funcName.equals("MPI_Ibcast");
}

inline bool isScatter(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Scatter") || funcName.equals("MPI_Scatterv") ||
         funcName.equals("MPI_Iscatter") || funcName.equals("MPI_Iscatterv");
}

inline bool isGather(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Gather") || funcName.equals("MPI_Gatherv") ||
         funcName.equals("MPI_Igather") || funcName.equals("MPI_Igatherv");
}

inline bool isAllgather(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Allgather") ||
         funcName.equals("MPI_Allgatherv") ||
         funcName.equals("MPI_Iallgather") ||
         funcName.equals("MPI_Iallgatherv");
}

inline bool isAlltoall(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Alltoall") || funcName.equals("MPI_Alltoallv") ||
         funcName.equals("MPI_Alltoallw") || funcName.equals("MPI_Ialltoall") ||
         funcName.equals("MPI_Ialltoallv") || funcName.equals("MPI_Ialltoallw");
}

inline bool isReduce(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Reduce") || funcName.equals("MPI_Ireduce");
}

inline bool isAllreduce(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Allreduce") || funcName.equals("MPI_Iallreduce");
}

inline bool isReduceScatter(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Reduce_scatter") ||
         funcName.equals("MPI_Reduce_scatter_block") ||
         funcName.equals("MPI_Ireduce_scatter") ||
         funcName.equals("MPI_Ireduce_scatter_block");
}

inline bool isScan(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Scan") || funcName.equals("MPI_Iscan") ||
         funcName.equals("MPI_Exscan") || funcName.equals("MPI_Iexscan");
}

inline bool isBlockingCollective(const llvm::StringRef &funcName) {
  return (isBcast(funcName) || isScatter(funcName) || isGather(funcName) ||
          isAllgather(funcName) || isAlltoall(funcName) || isReduce(funcName) ||
          isAllreduce(funcName) || isReduceScatter(funcName) ||
          isScan(funcName)) &&
         !funcName.startswith("MPI_I");
}

inline bool isNonBlockingCollective(const llvm::StringRef &funcName) {
  return (isBcast(funcName) || isScatter(funcName) || isGather(funcName) ||
          isAllgather(funcName) || isAlltoall(funcName) || isReduce(funcName) ||
          isAllreduce(funcName) || isReduceScatter(funcName) ||
          isScan(funcName)) &&
         funcName.startswith("MPI_I");
}

// ============================================================================
// One-Sided Communication (RMA - Remote Memory Access)
// ============================================================================

inline bool isWinCreate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_create") ||
         funcName.equals("MPI_Win_allocate") ||
         funcName.equals("MPI_Win_create_dynamic") ||
         funcName.equals("MPI_Win_allocate_shared");
}

inline bool isWinFree(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_free");
}

inline bool isPut(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Put") || funcName.equals("MPI_Rput");
}

inline bool isGet(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Get") || funcName.equals("MPI_Rget");
}

inline bool isAccumulate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Accumulate") ||
         funcName.equals("MPI_Raccumulate") ||
         funcName.equals("MPI_Get_accumulate") ||
         funcName.equals("MPI_Rget_accumulate") ||
         funcName.equals("MPI_Fetch_and_op") ||
         funcName.equals("MPI_Compare_and_swap");
}

// ============================================================================
// RMA Synchronization - Active Target (Fence)
// ============================================================================

inline bool isWinFence(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_fence");
}

// ============================================================================
// RMA Synchronization - Passive Target (Lock/Unlock)
// ============================================================================

inline bool isWinLock(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_lock") || funcName.equals("MPI_Win_lock_all");
}

inline bool isWinUnlock(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_unlock") ||
         funcName.equals("MPI_Win_unlock_all");
}

inline bool isWinFlush(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_flush") ||
         funcName.equals("MPI_Win_flush_all") ||
         funcName.equals("MPI_Win_flush_local") ||
         funcName.equals("MPI_Win_flush_local_all");
}

inline bool isWinSync(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_sync");
}

// ============================================================================
// RMA Synchronization - General Purpose (PSCW)
// ============================================================================

inline bool isWinPost(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_post");
}

inline bool isWinStart(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_start");
}

inline bool isWinComplete(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_complete");
}

inline bool isWinWait(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_wait");
}

inline bool isWinTest(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Win_test");
}

// ============================================================================
// Communicator and Group Management
// ============================================================================

inline bool isCommDup(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Comm_dup") ||
         funcName.equals("MPI_Comm_dup_with_info") ||
         funcName.equals("MPI_Comm_idup");
}

inline bool isCommSplit(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Comm_split") ||
         funcName.equals("MPI_Comm_split_type");
}

inline bool isCommFree(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Comm_free");
}

inline bool isCommCreate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Comm_create") ||
         funcName.equals("MPI_Comm_create_group");
}

// ============================================================================
// Request Management
// ============================================================================

inline bool isRequestFree(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Request_free");
}

inline bool isCancel(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cancel");
}

// ============================================================================
// Helper Functions - Classification
// ============================================================================

// Check if this is any wait operation
inline bool isAnyWait(const llvm::StringRef &funcName) {
  return isWait(funcName) || isWaitall(funcName) || isWaitany(funcName) ||
         isWaitsome(funcName);
}

// Check if this is any test operation
inline bool isAnyTest(const llvm::StringRef &funcName) {
  return isTest(funcName) || isTestall(funcName) || isTestany(funcName) ||
         isTestsome(funcName);
}

// Check if collective operation (implies synchronization across all processes)
inline bool isCollective(const llvm::StringRef &funcName) {
  return isBarrier(funcName) || isBcast(funcName) || isScatter(funcName) ||
         isGather(funcName) || isAllgather(funcName) || isAlltoall(funcName) ||
         isReduce(funcName) || isAllreduce(funcName) ||
         isReduceScatter(funcName) || isScan(funcName);
}

// Check if non-blocking operation (starts with MPI_I or is non-blocking RMA)
inline bool isNonBlocking(const llvm::StringRef &funcName) {
  return isIsend(funcName) || isIrecv(funcName) || isIprobe(funcName) ||
         funcName.startswith(
             "MPI_I") || // Most non-blocking ops start with MPI_I
         funcName.equals("MPI_Rput") ||
         funcName.equals("MPI_Rget") || funcName.equals("MPI_Raccumulate") ||
         funcName.equals("MPI_Rget_accumulate");
}

// Check if RMA data operation
inline bool isRMAOperation(const llvm::StringRef &funcName) {
  return isPut(funcName) || isGet(funcName) || isAccumulate(funcName);
}

// Check if RMA synchronization operation
inline bool isRMASync(const llvm::StringRef &funcName) {
  return isWinFence(funcName) || isWinLock(funcName) || isWinUnlock(funcName) ||
         isWinFlush(funcName) || isWinSync(funcName) || isWinPost(funcName) ||
         isWinStart(funcName) || isWinComplete(funcName) ||
         isWinWait(funcName) || isWinTest(funcName);
}

// Check if blocking point-to-point operation
inline bool isBlockingP2P(const llvm::StringRef &funcName) {
  return isSend(funcName) || isRecv(funcName) || isSendrecv(funcName) ||
         isProbe(funcName);
}

// Check if any MPI function (for debug/stats)
inline bool isMPI(const llvm::StringRef &funcName) {
  return funcName.startswith("MPI_") || funcName.startswith("PMPI_") ||
         funcName.startswith("ompi_mpi_") || funcName.startswith("mpi_") ||
         funcName.startswith("pmpi_");
}

// ============================================================================
// MPI Process Topology - Cartesian (MPI-2.2+)
// ============================================================================

inline bool isCartCreate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_create");
}

inline bool isCartDimsCreate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_dims_create");
}

inline bool isCartGet(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_get");
}

inline bool isCartShift(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_shift");
}

inline bool isCartCoords(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_coords");
}

inline bool isCartRank(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_rank");
}

inline bool isCartSub(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Cart_sub");
}

// ============================================================================
// MPI Process Topology - Distributed Graph (MPI-2.2+)
// ============================================================================

inline bool isDistGraphCreate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Dist_graph_create");
}

inline bool isDistGraphCreateAdjacent(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Dist_graph_create_adjacent");
}

inline bool isDistGraphNeighbors(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Dist_graph_neighbors");
}

inline bool isDistGraphNeighborsCount(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Dist_graph_neighbors_count");
}

// ============================================================================
// MPI Process Topology - Legacy Graph
// ============================================================================

inline bool isGraphCreate(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Graph_create");
}

inline bool isGraphGet(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Graph_get");
}

inline bool isGraphNeighbors(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Graph_neighbors");
}

inline bool isGraphNeighborsCount(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Graph_neighbors_count");
}

inline bool isGraphDimsGet(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Graphdims_get");
}

inline bool isGraphMap(const llvm::StringRef &funcName) {
  return funcName.equals("MPI_Graph_map");
}

// Check if any topology operation
inline bool isTopology(const llvm::StringRef &funcName) {
  return isCartCreate(funcName) || isCartDimsCreate(funcName) ||
         isCartGet(funcName) || isCartShift(funcName) ||
         isCartCoords(funcName) || isCartRank(funcName) ||
         isCartSub(funcName) || isDistGraphCreate(funcName) ||
         isDistGraphCreateAdjacent(funcName) ||
         isDistGraphNeighbors(funcName) ||
         isDistGraphNeighborsCount(funcName) || isGraphCreate(funcName) ||
         isGraphGet(funcName) || isGraphNeighbors(funcName) ||
         isGraphNeighborsCount(funcName) || isGraphDimsGet(funcName) ||
         isGraphMap(funcName);
}

} // namespace MPIModel
