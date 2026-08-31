/**
 * @file ThreadAPI.h
 * @brief Thread API Recognition and Analysis
 *
 * This file provides utilities for recognizing and analyzing thread-related
 * API calls in multithreaded programs. It supports pthread, OpenMP, and
 * custom threading libraries through a configurable API mapping system.
 *
 * Key Features:
 * - Recognition of thread creation, joining, and termination
 * - Lock acquisition and release operations
 * - Condition variable signaling and waiting
 * - Barrier synchronization support
 * - Configurable API mapping for different threading libraries
 *
 * @author rainoftime
 * @date 2025-2026
 * @ingroup Concurrency
 */

#ifndef THREADAPI_H
#define THREADAPI_H

#include "llvm/ADT/StringMap.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "Concurrency/ConcurrencyConfig.h"
#include "Concurrency/LinuxKernel/LinuxKernelSemanticRegistry.h"
#include "Concurrency/Utils/CUDA.h"
#include "Concurrency/Utils/CppThreading.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Do NOT use `using namespace llvm` in headers — it pollutes every TU that
// includes this file.  All LLVM types are qualified explicitly below.

using u32_t = unsigned;

/**
 * @class ThreadAPI
 * @brief Provides interfaces for recognizing and analyzing pthread/OpenMP
 * programs
 *
 * ThreadAPI is a singleton class that maps function names to thread API types.
 * It enables static analysis of multithreaded programs by identifying:
 * - Thread creation and synchronization points
 * - Lock acquire and release operations
 * - Condition variable operations
 * - Barrier synchronization
 *
 * @note Use ThreadAPI::getThreadAPI() to obtain the singleton instance
 * @note API mappings can be extended via configuration files
 */
class ThreadAPI {

public:
  enum class RuntimeLibrary {
    Unknown,
    PThread,
    OpenMP,
    MPI,
    Cpp,
    CUDA,
    LinuxKernel,
    Hare,
    Custom
  };

  enum class LockSemanticKind { None, Shared, Exclusive, Release };
  enum class LockOwnershipEffect {
    Acquire,
    Deferred,
    Try,
    Adopt,
    Release,
    Transfer,
    Detach,
    Swap
  };
  enum class TryLockSuccess { Unknown, Zero, NonZero };

  struct LockSemanticInfo {
    const llvm::Value *identity = nullptr;
    llvm::SmallVector<const llvm::Value *, 4> identities;
    LockSemanticKind kind = LockSemanticKind::None;
    LockOwnershipEffect ownership = LockOwnershipEffect::Acquire;
    TryLockSuccess try_success = TryLockSuccess::Unknown;
    bool is_try = false;
    bool conditional = false;

    bool isShared() const { return kind == LockSemanticKind::Shared; }
    bool isExclusive() const { return kind == LockSemanticKind::Exclusive; }
    bool isRelease() const { return kind == LockSemanticKind::Release; }
  };

  /**
   * @enum TD_TYPE
   * @brief Thread API function types
   *
   * Enumeration of all supported thread API operation types.
   * Used to classify function calls during static analysis.
   */
  enum TD_TYPE {
    TD_DUMMY = 0,     ///< Unknown or unrecognized API call
    TD_FORK,          ///< Create a new thread (e.g., pthread_create)
    TD_JOIN,          ///< Wait for a thread to join (e.g., pthread_join)
    TD_DETACH,        ///< Detach a thread (e.g., pthread_detach)
    TD_ACQUIRE,       ///< Acquire a lock (e.g., pthread_mutex_lock)
    TD_TRY_ACQUIRE,   ///< Try to acquire a lock without blocking (e.g.,
                      ///< pthread_mutex_trylock)
    TD_RWLOCK_RDLOCK, ///< Acquire read lock (e.g., pthread_rwlock_rdlock)
    TD_RWLOCK_WRLOCK, ///< Acquire write lock (e.g., pthread_rwlock_wrlock)
    TD_RELEASE,       ///< Release a lock (e.g., pthread_mutex_unlock)
    TD_EXIT,          ///< Exit/kill a thread (e.g., pthread_exit)
    TD_CANCEL,        ///< Cancel a thread by another (e.g., pthread_cancel)
    TD_COND_WAIT,   ///< Wait on a condition variable (e.g., pthread_cond_wait)
    TD_COND_SIGNAL, ///< Signal a condition variable (e.g., pthread_cond_signal)
    TD_COND_BROADCAST,  ///< Broadcast a condition variable (e.g.,
                        ///< pthread_cond_broadcast)
    TD_MUTEX_INI,       ///< Initialize a mutex
    TD_MUTEX_DESTROY,   ///< Destroy a mutex
    TD_CONDVAR_INI,     ///< Initialize a condition variable
    TD_CONDVAR_DESTROY, ///< Destroy a condition variable
    TD_BAR_INIT,        ///< Initialize a barrier
    TD_BAR_WAIT,        ///< Wait on a barrier
    HARE_PAR_FOR,       ///< Hare parallel for loop construct

    // C++11/17/20 Modern Synchronization Primitives
    TD_SHARED_RDLOCK, ///< std::shared_mutex::lock_shared (read lock)
    TD_SHARED_WRLOCK, ///< std::shared_mutex::lock (exclusive/write lock)
    TD_SHARED_UNLOCK, ///< std::shared_mutex::unlock[_shared]
    TD_CALL_ONCE,     ///< std::call_once - singleton initialization
    TD_FUTURE_GET,    ///< std::future::get - synchronization point
    TD_FUTURE_WAIT,   ///< std::future::wait - wait without getting value
    TD_PROMISE_SET,   ///< std::promise::set_value/set_exception
    TD_ASYNC,         ///< std::async - task creation

    // RAII Lock Wrappers (special handling needed)
    TD_LOCK_GUARD_CTOR,    ///< std::lock_guard constructor (acquire)
    TD_LOCK_GUARD_DTOR,    ///< std::lock_guard destructor (release)
    TD_UNIQUE_LOCK_CTOR,   ///< std::unique_lock constructor (acquire)
    TD_UNIQUE_LOCK_DTOR,   ///< std::unique_lock destructor (release)
    TD_UNIQUE_LOCK_LOCK,   ///< std::unique_lock::lock (manual acquire)
    TD_UNIQUE_LOCK_UNLOCK, ///< std::unique_lock::unlock (manual release)
    TD_SCOPED_LOCK_CTOR,   ///< std::scoped_lock constructor (acquire multiple)
    TD_SCOPED_LOCK_DTOR,   ///< std::scoped_lock destructor (release multiple)
    TD_SHARED_LOCK_CTOR,   ///< std::shared_lock constructor (shared acquire)
    TD_SHARED_LOCK_DTOR,   ///< std::shared_lock destructor (shared release)
    TD_CPP_LOCK_TRY,       ///< unique_lock/shared_lock nonblocking acquisition

    // C++20 Additional Primitives
    TD_JTHREAD_FORK,          ///< std::jthread constructor (fork)
    TD_JTHREAD_JOIN,          ///< std::jthread::join
    TD_JTHREAD_DTOR,          ///< std::jthread destructor auto-join
    TD_ATOMIC_WAIT,           ///< std::atomic::wait
    TD_ATOMIC_NOTIFY_ONE,     ///< std::atomic::notify_one
    TD_ATOMIC_NOTIFY_ALL,     ///< std::atomic::notify_all
    TD_LATCH_COUNT_DOWN,      ///< std::latch::count_down
    TD_LATCH_WAIT,            ///< std::latch::wait
    TD_LATCH_ARRIVE_WAIT,     ///< std::latch::arrive_and_wait
    TD_BARRIER_ARRIVE_WAIT,   ///< std::barrier::arrive_and_wait
    TD_BARRIER_ARRIVE,        ///< std::barrier::arrive
    TD_BARRIER_WAIT_CPP20,    ///< std::barrier::wait (C++20 version)
    TD_SEMAPHORE_ACQUIRE,     ///< std::counting_semaphore::acquire
    TD_SEMAPHORE_RELEASE,     ///< std::counting_semaphore::release
    TD_SEMAPHORE_TRY_ACQUIRE, ///< std::counting_semaphore::try_acquire

    // OpenMP Task Support (3.0+)
    TD_OMP_TASK,            ///< __kmpc_omp_task - explicit task creation
    TD_OMP_TASKWAIT,        ///< __kmpc_omp_taskwait - wait for child tasks
    TD_OMP_TASKWAIT_DEPS,   ///< __kmpc_omp_wait_deps* - partial task dependency
                            ///< wait
    TD_OMP_TASKYIELD,       ///< __kmpc_omp_taskyield - yield to other tasks
    TD_OMP_TASKGROUP_START, ///< __kmpc_taskgroup - start task group
    TD_OMP_TASKGROUP_END,   ///< __kmpc_end_taskgroup - end task group
    TD_OMP_TASK_WITH_DEPS,  ///< __kmpc_omp_task_with_deps - task with
                            ///< dependencies
    TD_OMP_TASKLOOP,        ///< __kmpc_taskloop - taskloop construct
    TD_OMP_TASK_COMPLETE,   ///< detached/inline task completion callback
    TD_OMP_SINGLE_START,    ///< __kmpc_single - single region entry
    TD_OMP_SINGLE_END,      ///< __kmpc_end_single - implicit single barrier
    TD_OMP_MASTER_START,    ///< __kmpc_master - master region entry
    TD_OMP_MASTER_END,      ///< __kmpc_end_master - master region exit
    TD_OMP_ORDERED_START,   ///< __kmpc_ordered - ordered region entry
    TD_OMP_ORDERED_END,     ///< __kmpc_end_ordered - ordered region exit
    TD_OMP_REDUCE_START,    ///< __kmpc_reduce - reduction with implicit barrier
    TD_OMP_REDUCE_END,      ///< __kmpc_end_reduce - reduction region exit
    TD_OMP_REDUCE_NOWAIT_START, ///< __kmpc_reduce_nowait - reduction without
                                ///< barrier
    TD_OMP_REDUCE_NOWAIT_END,   ///< __kmpc_end_reduce_nowait - reduction-nowait
                                ///< exit
    TD_OMP_FOR_STATIC_INIT,     ///< __kmpc_for_static_init_* - worksharing loop
                                ///< entry
    TD_OMP_FOR_STATIC_FINI,   ///< __kmpc_for_static_fini - worksharing loop end
    TD_OMP_FOR_DISPATCH_INIT, ///< __kmpc_dispatch_init_* - dynamic loop entry
    TD_OMP_FOR_DISPATCH_NEXT, ///< __kmpc_dispatch_next_* - loop chunk fetch
    TD_OMP_FOR_DISPATCH_FINI, ///< __kmpc_dispatch_fini_* - dynamic loop end

    // OpenMP Additional Constructs
    TD_OMP_SECTIONS_INIT,     ///< __kmpc_sections_init - sections construct
    TD_OMP_SECTIONS_NEXT,     ///< __kmpc_next_section - get next section
    TD_OMP_SECTIONS_END,      ///< __kmpc_end_sections - end sections
    TD_OMP_ATOMIC_START,      ///< __kmpc_atomic_start - atomic region start
    TD_OMP_ATOMIC_END,        ///< __kmpc_atomic_end - atomic region end
    TD_OMP_FLUSH,             ///< __kmpc_flush - memory fence
    TD_OMP_CANCEL,            ///< __kmpc_cancel - cancellation
    TD_OMP_CRITICAL_START,    ///< __kmpc_critical - critical section entry
    TD_OMP_CRITICAL_END,      ///< __kmpc_end_critical - critical section exit
    TD_OMP_PARALLEL_START,    ///< __kmpc_fork_call - parallel region entry
    TD_OMP_TARGET,            ///< __tgt_target* - target offloading
    TD_OMP_TARGET_DATA_BEGIN, ///< __tgt_target_data_begin
    TD_OMP_TARGET_DATA_END,   ///< __tgt_target_data_end
    TD_OMP_TARGET_DATA_UPDATE,

    // OpenMP 5.0+ Teams and Distribute
    TD_OMP_TEAMS,               ///< __kmpc_teams* - teams construct
    TD_OMP_TEAMS_HOST,          ///< __kmpc_teams_host
    TD_OMP_TEAMS_DISTRIBUTE,    ///< __kmpc_teams_distribute*
    TD_OMP_DISTRIBUTE,          ///< __kmpc_distribute* - distribute construct
    TD_OMP_DISTRIBUTE_STATIC,   ///< __kmpc_distribute_static*
    TD_OMP_DISTRIBUTE_DYNAMIC,  ///< __kmpc_distribute_dynamic*
    TD_OMP_DISTRIBUTE_GUIDANCE, ///< __kmpc_distribute_guidance*

    // OpenMP 5.0+ Loop
    TD_OMP_LOOP_STATIC_INIT,   ///< __kmpc_loop_static
    TD_OMP_LOOP_DYNAMIC_INIT,  ///< __kmpc_loop_dynamic
    TD_OMP_LOOP_GUIDANCE_INIT, ///< __kmpc_loop_guidance

    // OpenMP 5.0+ Affinity
    TD_OMP_AFFINITY, ///< __kmpc_affinity*

    // OpenMP 5.0+ Scope
    TD_OMP_SCOPE_START, ///< __kmpc_scope
    TD_OMP_SCOPE_END,   ///< __kmpc_end_scope

    // OpenMP 5.0+ Taskloop variants
    TD_OMP_TASKLOOP_SIMD, ///< __kmpc_taskloop_simd
    TD_OMP_TASKLOOP_FINI, ///< __kmpc_taskloop_fini

    // OpenMP 5.0+ Interop
    TD_OMP_INTEROP_INIT, ///< __kmpc_interop*
    TD_OMP_INTEROP_FINI, ///< __kmpc_interop_fini*

    // OpenMP 5.1+ Doacross
    TD_OMP_DOACROSS_INIT,   ///< __kmpc_doacross*
    TD_OMP_DOACROSS_WAIT,   ///< __kmpc_doacross_wait*
    TD_OMP_DOACROSS_SUBMIT, ///< __kmpc_doacross_submit*

    // CUDA / NVVM
    TD_CUDA_KERNEL_LAUNCH,  ///< CUDA kernel launch configuration or launch
    TD_CUDA_MULTI_DEVICE_LAUNCH, ///< Aggregate multi-device launch records
    TD_CUDA_DEVICE_SYNC,    ///< cudaDeviceSynchronize/cudaThreadSynchronize
    TD_CUDA_BARRIER,        ///< __syncthreads / llvm.nvvm.barrier0
    TD_CUDA_WARP_BARRIER,   ///< __syncwarp / llvm.nvvm.bar.warp.sync
    TD_CUDA_MEMORY_BARRIER, ///< threadfence / nvvm membar intrinsics
    TD_CUDA_ATOMIC,
    TD_CUDA_MEMCPY,
    TD_CUDA_MEMSET,
    TD_CUDA_MALLOC,
    TD_CUDA_FREE,
    TD_CUDA_UNIFIED_MEMORY,
    TD_CUDA_STREAM,
    TD_CUDA_EVENT,
    TD_CUDA_TEXTURE,
    TD_CUDA_SURFACE,
    TD_CUDA_DEVICE_MGMT,
    TD_CUDA_ERROR,

    // Linux call-form atomic operations (recognized, metadata-only lowering)
    TD_KERNEL_ATOMIC_READ,
    TD_KERNEL_ATOMIC_WRITE,
    TD_KERNEL_ATOMIC_RMW,

    // MPI Session Management (MPI-4.0)
    TD_MPI_SESSION_INIT,             ///< MPI_Session_init
    TD_MPI_SESSION_FINALIZE,         ///< MPI_Session_finalize
    TD_MPI_SESSION_GET_INFO,         ///< MPI_Session_get_info
    TD_MPI_SESSION_GET_NUM_ERRCODES, ///< MPI_Session_get_num_errcodes
    TD_MPI_SESSION_GET_ERRHANDLER,   ///< MPI_Session_get_errhandler
    TD_MPI_SESSION_SET_ERRHANDLER,   ///< MPI_Session_set_errhandler

    // MPI Error Handling
    TD_MPI_ERRHANDLER_CREATE,    ///< MPI_Errhandler_create
    TD_MPI_ERRHANDLER_FREE,      ///< MPI_Errhandler_free
    TD_MPI_COMM_GET_ERRHANDLER,  ///< MPI_Comm_get_errhandler
    TD_MPI_COMM_SET_ERRHANDLER,  ///< MPI_Comm_set_errhandler
    TD_MPI_COMM_CALL_ERRHANDLER, ///< MPI_Comm_call_errhandler
    TD_MPI_WIN_GET_ERRHANDLER,   ///< MPI_Win_get_errhandler
    TD_MPI_WIN_SET_ERRHANDLER,   ///< MPI_Win_set_errhandler
    TD_MPI_FILE_GET_ERRHANDLER,  ///< MPI_File_get_errhandler
    TD_MPI_FILE_SET_ERRHANDLER,  ///< MPI_File_set_errhandler
    TD_MPI_ERROR_CLASS,          ///< MPI_Error_class
    TD_MPI_ERROR_STRING,         ///< MPI_Error_string

    // MPI Info Management
    TD_MPI_INFO_CREATE,       ///< MPI_Info_create
    TD_MPI_INFO_DUP,          ///< MPI_Info_dup
    TD_MPI_INFO_FREE,         ///< MPI_Info_free
    TD_MPI_INFO_GET,          ///< MPI_Info_get
    TD_MPI_INFO_GET_VALUELEN, ///< MPI_Info_get_valuelen
    TD_MPI_INFO_GET_NKEYS,    ///< MPI_Info_get_nkeys
    TD_MPI_INFO_GET_NTHKEY,   ///< MPI_Info_get_nthkey
    TD_MPI_INFO_GET_KEYVAL,   ///< MPI_Info_get_keyval
    TD_MPI_INFO_SET,          ///< MPI_Info_set
    TD_MPI_INFO_DELETE,       ///< MPI_Info_delete
    TD_MPI_INFO_C2F,          ///< MPI_Info_c2f
    TD_MPI_INFO_CREATE_ENV,   ///< MPI_Info_create_env
    TD_MPI_INFO_FREE_ENV,     ///< MPI_Info_free_env

    // MPI Buffer Query Operations
    TD_MPI_GET_COUNT,             ///< MPI_Get_count
    TD_MPI_GET_ELEMENTS,          ///< MPI_Get_elements
    TD_MPI_GET_ELEMENTS_X,        ///< MPI_Get_elements_x
    TD_MPI_STATUS_SIZE,           ///< MPI_Status_size
    TD_MPI_STATUS_SET_ELEMENTS,   ///< MPI_Status_set_elements
    TD_MPI_STATUS_SET_ELEMENTS_X, ///< MPI_Status_set_elements_x

    // MPI Message Matching (MPI-3.0)
    TD_MPI_MPROBE,  ///< MPI_Mprobe
    TD_MPI_IMPROBE, ///< MPI_Improbe
    TD_MPI_IMRECV,  ///< MPI_Imrecv
    TD_MPI_MRECV,   ///< MPI_Mrecv

    // MPI Process Management
    TD_MPI_INIT,     ///< MPI_Init, MPI_Init_thread
    TD_MPI_FINALIZE, ///< MPI_Finalize

    // MPI Point-to-Point (blocking = synchronization point)
    TD_MPI_SEND,     ///< MPI_Send, MPI_Ssend, MPI_Bsend, MPI_Rsend
    TD_MPI_RECV,     ///< MPI_Recv
    TD_MPI_SENDRECV, ///< MPI_Sendrecv, MPI_Sendrecv_replace
    TD_MPI_PROBE,    ///< MPI_Probe

    // MPI Point-to-Point (non-blocking)
    TD_MPI_ISEND,  ///< MPI_Isend, MPI_Issend, MPI_Ibsend, MPI_Irsend
    TD_MPI_IRECV,  ///< MPI_Irecv
    TD_MPI_IPROBE, ///< MPI_Iprobe
    TD_MPI_PERSISTENT_SEND_INIT,
    TD_MPI_PERSISTENT_RECV_INIT,
    TD_MPI_REQUEST_START,

    // MPI Synchronization
    TD_MPI_WAIT,     ///< MPI_Wait (join-like for non-blocking ops)
    TD_MPI_WAITALL,  ///< MPI_Waitall
    TD_MPI_WAITANY,  ///< MPI_Waitany
    TD_MPI_WAITSOME, ///< MPI_Waitsome
    TD_MPI_TEST,     ///< MPI_Test
    TD_MPI_TESTALL,  ///< MPI_Testall
    TD_MPI_TESTANY,  ///< MPI_Testany
    TD_MPI_TESTSOME, ///< MPI_Testsome
    TD_MPI_BARRIER,  ///< MPI_Barrier, MPI_Ibarrier

    // MPI Collectives (all are synchronization points)
    TD_MPI_BCAST,     ///< MPI_Bcast, MPI_Ibcast
    TD_MPI_SCATTER,   ///< MPI_Scatter, MPI_Scatterv, MPI_I*
    TD_MPI_GATHER,    ///< MPI_Gather, MPI_Gatherv, MPI_I*
    TD_MPI_ALLGATHER, ///< MPI_Allgather, MPI_Allgatherv, MPI_I*
    TD_MPI_ALLTOALL,  ///< MPI_Alltoall, MPI_Alltoallv, MPI_Alltoallw, MPI_I*
    TD_MPI_REDUCE,    ///< MPI_Reduce, MPI_Ireduce
    TD_MPI_ALLREDUCE, ///< MPI_Allreduce, MPI_Iallreduce
    TD_MPI_REDUCE_SCATTER, ///< MPI_Reduce_scatter, MPI_Reduce_scatter_block,
                           ///< MPI_I*
    TD_MPI_SCAN,           ///< MPI_Scan, MPI_Exscan, MPI_I*

    // MPI One-Sided (RMA - Remote Memory Access)
    TD_MPI_WIN_CREATE, ///< MPI_Win_create, MPI_Win_allocate,
                       ///< MPI_Win_create_dynamic
    TD_MPI_WIN_FREE,   ///< MPI_Win_free
    TD_MPI_PUT,        ///< MPI_Put, MPI_Rput (shared write)
    TD_MPI_GET,        ///< MPI_Get, MPI_Rget (shared read)
    TD_MPI_ACCUMULATE, ///< MPI_Accumulate, MPI_Get_accumulate,
                       ///< MPI_Fetch_and_op, etc. (atomic RMW)

    // MPI RMA Synchronization - Active Target
    TD_MPI_WIN_FENCE, ///< MPI_Win_fence (barrier for RMA)

    // MPI RMA Synchronization - Passive Target
    TD_MPI_WIN_LOCK,   ///< MPI_Win_lock, MPI_Win_lock_all (RMA lock)
    TD_MPI_WIN_UNLOCK, ///< MPI_Win_unlock, MPI_Win_unlock_all (RMA unlock)
    TD_MPI_WIN_FLUSH,  ///< MPI_Win_flush, MPI_Win_flush_all,
                       ///< MPI_Win_flush_local* (RMA completion)
    TD_MPI_WIN_SYNC,   ///< MPI_Win_sync (memory consistency)

    // MPI RMA Synchronization - General Purpose (PSCW)
    TD_MPI_WIN_POST,     ///< MPI_Win_post (exposure epoch start)
    TD_MPI_WIN_START,    ///< MPI_Win_start (access epoch start)
    TD_MPI_WIN_COMPLETE, ///< MPI_Win_complete (access epoch end)
    TD_MPI_WIN_WAIT,     ///< MPI_Win_wait (exposure epoch end)
    TD_MPI_WIN_TEST,     ///< MPI_Win_test (test exposure epoch)

    // MPI Communicator Management
    TD_MPI_COMM_DUP,    ///< MPI_Comm_dup, MPI_Comm_idup
    TD_MPI_COMM_SPLIT,  ///< MPI_Comm_split, MPI_Comm_split_type
    TD_MPI_COMM_CREATE, ///< MPI_Comm_create, MPI_Comm_create_group
    TD_MPI_COMM_FREE,   ///< MPI_Comm_free

    // MPI Request Management
    TD_MPI_REQUEST_FREE, ///< MPI_Request_free
    TD_MPI_CANCEL,       ///< MPI_Cancel

    // MPI Datatype Management
    TD_MPI_TYPE_CONTIGUOUS,      ///< MPI_Type_contiguous
    TD_MPI_TYPE_VECTOR,          ///< MPI_Type_vector
    TD_MPI_TYPE_HVECTOR,         ///< MPI_Type_hvector
    TD_MPI_TYPE_INDEXED,         ///< MPI_Type_indexed
    TD_MPI_TYPE_HINDEXED,        ///< MPI_Type_hindexed
    TD_MPI_TYPE_STRUCT,          ///< MPI_Type_struct
    TD_MPI_TYPE_CREATE_DLPACK,   ///< MPI_Type_create_dlpack (MPI-4.1)
    TD_MPI_TYPE_CREATE_SUBARRAY, ///< MPI_Type_create_subarray
    TD_MPI_TYPE_CREATE_DARRAY,   ///< MPI_Type_create_darray
    TD_MPI_TYPE_CREATE_RESIZED,  ///< MPI_Type_create_resized
    TD_MPI_TYPE_CREATE_HINDEXED, ///< MPI_Type_create_hindexed (legacy)
    TD_MPI_TYPE_CREATE_HVECTOR,  ///< MPI_Type_create_hvector (legacy)
    TD_MPI_TYPE_GET_EXTENT,      ///< MPI_Type_get_extent
    TD_MPI_TYPE_GET_TRUE_EXTENT, ///< MPI_Type_get_true_extent
    TD_MPI_TYPE_SIZE,            ///< MPI_Type_size
    TD_MPI_TYPE_COMMIT,          ///< MPI_Type_commit

    // MPI Process Topology (MPI-2.2+)
    TD_MPI_CART_CREATE,       ///< MPI_Cart_create - Cartesian topology
    TD_MPI_CART_DIMS_CREATE,  ///< MPI_Cart_dims_create - create dimension sizes
    TD_MPI_CART_GET,          ///< MPI_Cart_get - get Cartesian topology info
    TD_MPI_CART_SHIFT,        ///< MPI_Cart_shift - get shift source/dest
    TD_MPI_CART_COORDS,       ///< MPI_Cart_coords - get coords from rank
    TD_MPI_CART_RANK,         ///< MPI_Cart_rank - get rank from coords
    TD_MPI_CART_SUB,          ///< MPI_Cart_sub - create sub-grid
    TD_MPI_DIST_GRAPH_CREATE, ///< MPI_Dist_graph_create - distributed graph
    TD_MPI_DIST_GRAPH_CREATE_ADJACENT, ///< MPI_Dist_graph_create_adjacent
    TD_MPI_DIST_GRAPH_NEIGHBORS,       ///< MPI_Dist_graph_neighbors
    TD_MPI_DIST_GRAPH_NEIGHBORS_COUNT, ///< MPI_Dist_graph_neighbors_count
    TD_MPI_GRAPH_CREATE,    ///< MPI_Graph_create - deprecated but still used
    TD_MPI_GRAPH_GET,       ///< MPI_Graph_get
    TD_MPI_GRAPH_NEIGHBORS, ///< MPI_Graph_neighbors
    TD_MPI_GRAPH_NEIGHBORS_COUNT, ///< MPI_Graph_neighbors_count
    TD_MPI_GRAPH_DIMS_GET,        ///< MPI_Graphdims_get
    TD_MPI_GRAPH_MAP,             ///< MPI_Graph_map

    // Linux Kernel Spinlocks
    TD_KERNEL_SPIN_LOCK_INIT, ///< spin_lock_init, raw_spin_lock_init
    TD_KERNEL_SPIN_LOCK,      ///< spin_lock, spin_lock_irq, spin_lock_irqsave,
                              ///< spin_lock_bh
    TD_KERNEL_SPIN_UNLOCK,    ///< spin_unlock, spin_unlock_irq,
                              ///< spin_unlock_irqrestore, spin_unlock_bh
    TD_KERNEL_SPIN_TRYLOCK,   ///< spin_trylock, raw_spin_trylock

    // Linux Kernel Mutexes
    TD_KERNEL_MUTEX_INIT,    ///< mutex_init, __mutex_init
    TD_KERNEL_MUTEX_LOCK,    ///< mutex_lock, mutex_lock_interruptible,
                             ///< mutex_lock_killable
    TD_KERNEL_MUTEX_UNLOCK,  ///< mutex_unlock
    TD_KERNEL_MUTEX_TRYLOCK, ///< mutex_trylock

    // Linux Kernel Semaphores
    TD_KERNEL_SEMA_INIT, ///< sema_init, init_MUTEX, init_MUTEX_LOCKED
    TD_KERNEL_DOWN, ///< down, down_interruptible, down_killable, down_trylock
    TD_KERNEL_UP,   ///< up

    // Linux Kernel Read-Write Locks
    TD_KERNEL_READ_LOCK,    ///< read_lock, read_lock_irq, read_lock_irqsave,
                            ///< read_lock_bh
    TD_KERNEL_READ_UNLOCK,  ///< read_unlock, read_unlock_irq,
                            ///< read_unlock_irqrestore, read_unlock_bh
    TD_KERNEL_WRITE_LOCK,   ///< write_lock, write_lock_irq, write_lock_irqsave,
                            ///< write_lock_bh
    TD_KERNEL_WRITE_UNLOCK, ///< write_unlock, write_unlock_irq,
                            ///< write_unlock_irqrestore, write_unlock_bh

    // Linux Kernel Read-Write Semaphores
    TD_KERNEL_DOWN_READ,  ///< down_read, down_read_trylock
    TD_KERNEL_UP_READ,    ///< up_read
    TD_KERNEL_DOWN_WRITE, ///< down_write, down_write_trylock
    TD_KERNEL_UP_WRITE,   ///< up_write
    TD_KERNEL_INIT_RWSEM, ///< init_rwsem

    // Linux Kernel RCU (Read-Copy-Update)
    TD_KERNEL_RCU_READ_LOCK,   ///< rcu_read_lock, __rcu_read_lock
    TD_KERNEL_RCU_READ_UNLOCK, ///< rcu_read_unlock, __rcu_read_unlock
    TD_KERNEL_SYNCHRONIZE_RCU, ///< synchronize_rcu, synchronize_rcu_expedited,
                               ///< synchronize_srcu
    TD_KERNEL_CALL_RCU,        ///< call_rcu, call_srcu
    TD_KERNEL_RCU_DEREFERENCE, ///< rcu_dereference, rcu_dereference_check,
                               ///< rcu_dereference_protected
    TD_KERNEL_RCU_ASSIGN_POINTER, ///< rcu_assign_pointer

    // Linux Kernel Seq Locks
    TD_KERNEL_SEQLOCK_INIT,    ///< seqlock_init
    TD_KERNEL_READ_SEQBEGIN,   ///< read_seqbegin, read_seqbegin_irqsave
    TD_KERNEL_READ_SEQRETRY,   ///< read_seqretry, read_seqretry_irqrestore
    TD_KERNEL_WRITE_SEQLOCK,   ///< write_seqlock, write_seqlock_irq,
                               ///< write_seqlock_irqsave, write_seqlock_bh
    TD_KERNEL_WRITE_SEQUNLOCK, ///< write_sequnlock, write_sequnlock_irq,
                               ///< write_sequnlock_irqrestore,
                               ///< write_sequnlock_bh

    // Linux Kernel Completion Variables
    TD_KERNEL_INIT_COMPLETION,     ///< init_completion
    TD_KERNEL_WAIT_FOR_COMPLETION, ///< wait_for_completion,
                                   ///< wait_for_completion_interruptible,
                                   ///< wait_for_completion_killable,
                                   ///< wait_for_completion_timeout
    TD_KERNEL_COMPLETE,            ///< complete (wakes one waiter)
    TD_KERNEL_COMPLETE_ALL,        ///< complete_all (wakes all waiters)

    // Linux Kernel Wait Queues
    TD_KERNEL_INIT_WAITQUEUE_HEAD, ///< init_waitqueue_head
    TD_KERNEL_WAIT_EVENT,          ///< wait_event, wait_event_interruptible,
                                   ///< wait_event_killable, wait_event_timeout
    TD_KERNEL_WAKE_UP,         ///< wake_up, wake_up_interruptible, wake_up_nr,
                               ///< wake_up_all, wake_up_one
    TD_KERNEL_PREPARE_TO_WAIT, ///< prepare_to_wait, prepare_to_wait_exclusive
    TD_KERNEL_FINISH_WAIT,     ///< finish_wait

    // Linux Kernel Memory Barriers
    TD_KERNEL_MEMORY_BARRIER, ///< mb, rmb, wmb, smp_mb, smp_rmb, smp_wmb,
                              ///< barrier

    // Wrapper-state operations live at the end to keep legacy TD_TYPE values
    // stable for clients that persist or iterate the historical enum range.
    TD_CPP_LOCK_MOVE_CTOR,   ///< unique_lock/shared_lock ownership transfer
    TD_CPP_LOCK_MOVE_ASSIGN, ///< wrapper move assignment
    TD_CPP_LOCK_SWAP,        ///< wrapper state exchange
    TD_CPP_LOCK_RELEASE      ///< unique_lock::release ownership detach
  };

  struct APIDescription {
    TD_TYPE type = TD_DUMMY;
    RuntimeLibrary library = RuntimeLibrary::Unknown;
    std::string semantic_tag;
    std::vector<std::string> traits;
    TryLockSuccess success = TryLockSuccess::Unknown;
    bool conditional_acquire = false;
    bool from_config = false;
  };

  enum class SemanticLoweringKind { Modeled, Deferred, RecognizedButUnmodeled };

  enum class SemanticLoweringOwner : uint32_t {
    None = 0,
    LockSet = 1u << 0,
    MHP = 1u << 1,
    HB = 1u << 2,
    OpenMP = 1u << 3,
    MPI = 1u << 4,
    ExplicitFallback = 1u << 5
  };

  struct SemanticLoweringInfo {
    SemanticLoweringKind kind = SemanticLoweringKind::RecognizedButUnmodeled;
    const char *reason = "unclassified";
    uint32_t owners = 0;
  };

  enum class MatchKind { Exact, Prefix };

  enum class LockMode { Unknown, Shared, Exclusive };

  struct LockSemantics {
    bool is_lock_api = false;
    bool is_acquire = false;
    bool is_release = false;
    bool is_try = false;
    bool is_semaphore = false;
    bool is_binary_semaphore = false;
    LockMode mode = LockMode::Unknown;
  };

  /// Map type for API name to TD_TYPE conversion
  using TDAPIMap = llvm::StringMap<TD_TYPE>;

  /// Argument indices for TD_FORK (Goblint-style; default pthread_create:
  /// 0,2,3)
  struct ForkArgIndices {
    static constexpr unsigned NoArgument = ~0u;
    static constexpr unsigned ReturnValue = ~1u;
    unsigned thread_arg = 0;
    unsigned start_routine_arg = 2;
    unsigned arg_arg = 3;
    unsigned payload_begin = NoArgument;
    bool variadic_payload = false;
  };
  /// Argument indices for TD_JOIN (default pthread_join: 0,1)
  struct JoinArgIndices {
    static constexpr unsigned NoArgument = ~0u;
    static constexpr unsigned ReturnValue = ~1u;
    unsigned thread_arg = 0;
    unsigned ret_arg = 1;
  };
  ForkArgIndices getForkArgIndices(const llvm::Function *F) const;
  ForkArgIndices getForkArgIndices(const llvm::CallBase *cb) const;
  JoinArgIndices getJoinArgIndices(const llvm::Function *F) const;
  JoinArgIndices getJoinArgIndices(const llvm::CallBase *cb) const;

private:
  /// Return true if the callee is registered in the explicit API map/config.
  bool hasMappedAPIEntry(const llvm::Function *F) const;
  bool hasMappedAPIEntry(const llvm::CallBase *cb) const;

  /// Return true for std::thread-like constructors whose operand layout is not
  /// pthread-compatible.
  bool isCppThreadLikeFork(const llvm::Function *F) const;

  /// Return true if this std::async call is a definite asynchronous launch.
  bool isDefiniteAsyncLaunch(const llvm::Instruction *inst) const;

  /// Return true if this std::async call is provably deferred-only.
  bool isProvablyDeferredAsyncLaunch(const llvm::Instruction *inst) const;

  /// Return the first operand index where a callable/payload may appear for
  /// std::thread-like APIs.
  unsigned getCppForkCallableSearchStart(const llvm::Instruction *inst) const;

  /// Safely fetch a call operand, returning nullptr when the argument is
  /// absent.
  const llvm::Value *getCallArg(const llvm::Instruction *inst,
                                unsigned idx) const;

  const llvm::Value *getCallField(const llvm::Instruction *inst,
                                  unsigned field) const;

  /// Best-effort extraction of a direct callable passed to std::thread.
  const llvm::Value *getCppThreadCallable(const llvm::Instruction *inst) const;

  /// Return true when the callee is a std::condition_variable_any member.
  bool isConditionVariableAny(const llvm::Function *F) const;

  /// Return the mutex identity associated with a condition-variable wait.
  const llvm::Value *
  getConditionVariableWaitMutex(const llvm::Instruction *inst) const;

  /// Return the underlying mutex identity for C++ RAII lock wrappers.
  const llvm::Value *
  getCppWrapperUnderlyingLock(const llvm::Instruction *inst) const;

  llvm::SmallVector<const llvm::Value *, 4>
  getCppWrapperUnderlyingLocks(const llvm::Instruction *inst) const;

  const llvm::CallBase *
  getKernelThreadCreateCall(const llvm::Instruction *inst) const;

  const kernel::LinuxKernelAPISemantics *
  lookupKernelSemantics(llvm::StringRef name) const;
  bool isKernelOperation(const llvm::CallBase *call,
                         kernel::OperationKind kind) const;
  TD_TYPE getKernelType(llvm::StringRef name) const;

  bool cppWrapperDestructorDefinitelyReleases(
      const llvm::Instruction *inst) const;

  /// API map, from a string to threadAPI type
  TDAPIMap tdAPIMap;
  std::unordered_map<std::string, ForkArgIndices> m_fork_args;
  std::unordered_map<std::string, JoinArgIndices> m_join_args;
  std::unordered_map<std::string, APIDescription> m_api_descriptions;
  struct MatchRule {
    std::string pattern;
    MatchKind kind = MatchKind::Exact;
    APIDescription description;
  };
  std::vector<MatchRule> m_match_rules;

  /// Configuration for threading models
  concurrency::ConcurrencyConfig m_config;
  kernel::LinuxKernelSemanticRegistry m_kernel_api_registry;

  /// Constructor
  ThreadAPI() { init(); }

  /// Initialize the map
  void init();
  void reinitialize();

  /// Static reference
  static ThreadAPI *tdAPI;

  /// Load configuration from a file
  void loadConfig(const std::string &filename);
  void loadSemanticConfig(const std::string &filename);

  /// Add a new entry to the API map
  void addEntry(const std::string &name, TD_TYPE type);
  void addDescription(const std::string &name,
                      const APIDescription &description);
  void addMatchRule(const std::string &pattern, MatchKind kind,
                    const APIDescription &description);
  RuntimeLibrary inferLibrary(TD_TYPE type) const;
  bool isLibraryEnabled(RuntimeLibrary library) const;
  const APIDescription *lookupDescription(const llvm::Function *F) const;
  const APIDescription *lookupDescription(llvm::StringRef name) const;
  const MatchRule *lookupMatchRule(llvm::StringRef normalized_name) const;
  TD_TYPE getConfiguredType(llvm::StringRef normalized_name) const;
  TD_TYPE getTypeForName(llvm::StringRef name) const;

public:
  inline bool isSemaphoreType(TD_TYPE type) const {
    return type == TD_SEMAPHORE_ACQUIRE || type == TD_SEMAPHORE_RELEASE ||
           type == TD_SEMAPHORE_TRY_ACQUIRE;
  }

  /// Get the function type if it is a threadAPI function
  TD_TYPE getType(const llvm::Function *F) const;
  TD_TYPE getType(const llvm::CallBase *cb) const;

  /// Get the concurrency configuration
  const concurrency::ConcurrencyConfig &getConfig() const { return m_config; }

  /// Set the concurrency configuration
  void setConfig(const concurrency::ConcurrencyConfig &config) {
    m_config = config;
  }

  APIDescription describe(const llvm::Function *F) const;
  APIDescription describe(const llvm::CallBase *cb) const;
  RuntimeLibrary getRuntimeLibrary(const llvm::CallBase *cb) const {
    return describe(cb).library;
  }
  std::string getSemanticTag(const llvm::CallBase *cb) const {
    return describe(cb).semantic_tag;
  }
  RuntimeLibrary getRuntimeLibrary(const llvm::Function *F) const {
    return describe(F).library;
  }
  std::string getSemanticTag(const llvm::Function *F) const {
    return describe(F).semantic_tag;
  }
  static constexpr uint32_t
  semanticLoweringOwnerMask(SemanticLoweringOwner owner) {
    return static_cast<uint32_t>(owner);
  }
  static constexpr uint32_t
  semanticLoweringOwnerMask(SemanticLoweringOwner owner_a,
                            SemanticLoweringOwner owner_b) {
    return semanticLoweringOwnerMask(owner_a) |
           semanticLoweringOwnerMask(owner_b);
  }
  static constexpr uint32_t
  semanticLoweringOwnerMask(SemanticLoweringOwner owner_a,
                            SemanticLoweringOwner owner_b,
                            SemanticLoweringOwner owner_c) {
    return semanticLoweringOwnerMask(owner_a, owner_b) |
           semanticLoweringOwnerMask(owner_c);
  }
  SemanticLoweringInfo getSemanticLoweringInfo(TD_TYPE type) const;
  SemanticLoweringInfo getSemanticLoweringInfo(const llvm::Function *F) const;
  bool hasSemanticLoweringOwner(TD_TYPE type,
                                SemanticLoweringOwner owner) const {
    return (getSemanticLoweringInfo(type).owners &
            semanticLoweringOwnerMask(owner)) != 0;
  }
  bool hasSemanticLoweringOwner(const llvm::Function *F,
                                SemanticLoweringOwner owner) const {
    return (getSemanticLoweringInfo(F).owners &
            semanticLoweringOwnerMask(owner)) != 0;
  }
  bool hasSemanticTag(const llvm::Function *F, llvm::StringRef tag) const {
    return describe(F).semantic_tag == tag;
  }
  bool hasSemanticTag(const llvm::CallBase *cb, llvm::StringRef tag) const {
    return describe(cb).semantic_tag == tag;
  }
  bool semanticTagStartsWith(const llvm::Function *F,
                             llvm::StringRef prefix) const {
    return llvm::StringRef(describe(F).semantic_tag).startswith(prefix);
  }
  bool hasTrait(const llvm::Function *F, llvm::StringRef trait) const {
    const APIDescription description = describe(F);
    for (const std::string &entry : description.traits) {
      if (entry == trait) {
        return true;
      }
    }
    return false;
  }
  bool hasTrait(const llvm::CallBase *cb, llvm::StringRef trait) const {
    const APIDescription description = describe(cb);
    return llvm::is_contained(description.traits, trait.str());
  }

  /// Return a static reference to the singleton instance.
  static ThreadAPI *getThreadAPI() {
    if (tdAPI == nullptr) {
      tdAPI = new ThreadAPI();
    }
    return tdAPI;
  }

  /// Reset singleton state in place so retained ThreadAPI pointers stay valid.
  static void resetThreadAPI() {
    if (tdAPI == nullptr) {
      tdAPI = new ThreadAPI();
      return;
    }
    tdAPI->reinitialize();
  }

  /// Return the callee/callsite/func
  //@{
  const llvm::Function *getCallee(const llvm::Instruction *inst) const;

  const llvm::Function *getCallee(const llvm::CallBase *cb) const;

  const llvm::GlobalValue *getCalledGlobal(const llvm::CallBase *cb) const;

  llvm::StringRef getCalledAPIName(const llvm::CallBase *cb) const;

  const llvm::CallBase *getLLVMCallSite(const llvm::Instruction *inst) const;
  //@}

  inline const llvm::CallBase *
  getNextCallInBlock(const llvm::Instruction *inst) const {
    if (!inst) {
      return nullptr;
    }
    const llvm::Instruction *next = inst->getNextNode();
    while (next) {
      if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(next)) {
        return cb;
      }
      next = next->getNextNode();
    }
    return nullptr;
  }

  inline const llvm::Function *
  getCUDALaunchedKernel(const llvm::Instruction *inst) const {
    if (!inst) {
      return nullptr;
    }
    const llvm::Function *callee = getCallee(inst);
    if (!callee) {
      return nullptr;
    }
    if (CUDAModel::isLegacyKernelConfiguration(callee->getName())) {
      return getCallee(getNextCallInBlock(inst));
    }
    if (getType(callee) != TD_CUDA_KERNEL_LAUNCH) {
      return nullptr;
    }
    const CUDAModel::KernelLaunchLayout layout =
        CUDAModel::getKernelLaunchLayout(callee->getName());
    const llvm::Value *kernel = getCallArg(inst, layout.kernel_arg);
    return llvm::dyn_cast_or_null<llvm::Function>(
        kernel ? kernel->stripPointerCasts() : nullptr);
  }

  inline bool
  isCUDAKernelCallImmediatelyAfterLaunch(const llvm::Instruction *inst) const {
    if (!inst) {
      return false;
    }
    const llvm::Function *callee = getCallee(inst);
    const llvm::Instruction *prev = inst->getPrevNode();
    while (prev) {
      if (const auto *prev_call = llvm::dyn_cast<llvm::CallBase>(prev)) {
        const llvm::Function *prev_callee = getCallee(prev_call);
        if (getType(prev_callee) != TD_CUDA_KERNEL_LAUNCH) {
          return false;
        }
        return getCUDALaunchedKernel(prev) == callee;
      }
      prev = prev->getPrevNode();
    }
    return false;
  }

  /// Return true if this call create a new thread
  //@{
  inline bool isForkLike(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    if (cb && isKernelOperation(cb, kernel::OperationKind::KTHREAD_START))
      return getKernelThreadCreateCall(inst) != nullptr;
    return t == TD_FORK || t == TD_JTHREAD_FORK ||
           t == TD_CUDA_KERNEL_LAUNCH || t == TD_CUDA_MULTI_DEVICE_LAUNCH ||
           (callee && CUDAModel::isLegacyKernelConfiguration(callee->getName())) ||
           (t == TD_ASYNC && !isProvablyDeferredAsyncLaunch(inst));
  }
  inline bool isForkLike(const llvm::CallBase *cb) const {
    return isForkLike(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  inline bool isTDFork(const llvm::Instruction *inst) const {
    return isForkLike(inst);
  }
  inline bool isTDFork(const llvm::CallBase *cb) const {
    return isForkLike(cb);
  }
  //@}

  /// Return true if this call proceeds a hare_parallel_for
  //@{
  inline bool isHareParFor(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb && getType(cb) == HARE_PAR_FOR;
  }
  inline bool isHareParFor(const llvm::CallBase *cb) const {
    return isHareParFor(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return arguments/attributes of pthread_create / hare_parallel_for
  //@{
  /// Return the thread handle argument (configurable via thread.spec; default
  /// 0)
  inline const llvm::Value *
  getForkedThread(const llvm::Instruction *inst) const {
    if (!isForkLike(inst))
      return nullptr;
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE type = cb ? getType(cb) : getType(callee);
    if (cb && isKernelOperation(cb, kernel::OperationKind::KTHREAD_START))
      return getCallArg(inst, 0);
    if (type == TD_ASYNC || type == TD_CUDA_KERNEL_LAUNCH ||
        type == TD_CUDA_MULTI_DEVICE_LAUNCH ||
        (callee && CUDAModel::isLegacyKernelConfiguration(callee->getName())) ||
        (cb && getRuntimeLibrary(cb) == RuntimeLibrary::OpenMP))
      return nullptr;
    if (isCppThreadLikeFork(callee))
      return getCallArg(inst, 0);
    if (!hasMappedAPIEntry(cb))
      return nullptr;
    unsigned idx = getForkArgIndices(cb).thread_arg;
    return getCallField(inst, idx);
  }
  inline const llvm::Value *getForkedThread(const llvm::CallBase *cb) const {
    return getForkedThread(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// Return the start-routine argument (configurable; default 2)
  inline const llvm::Value *getForkedFun(const llvm::Instruction *inst) const {
    if (!isForkLike(inst))
      return nullptr;
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const TD_TYPE type = cb ? getType(cb) : getType(callee);
    if (cb && isKernelOperation(cb, kernel::OperationKind::KTHREAD_START)) {
      const llvm::CallBase *create = getKernelThreadCreateCall(inst);
      if (!create || create->arg_size() < 1)
        return nullptr;
      return create->getArgOperand(0)->stripPointerCasts();
    }
    if (isCppThreadLikeFork(callee))
      return getCppThreadCallable(inst);

    if (type == TD_CUDA_MULTI_DEVICE_LAUNCH)
      return nullptr;
    if (type == TD_CUDA_KERNEL_LAUNCH ||
        CUDAModel::isLegacyKernelConfiguration(callee->getName())) {
      return getCUDALaunchedKernel(inst);
    }

    if (cb && hasSemanticTag(cb, "fork")) {
      unsigned idx = hasMappedAPIEntry(cb)
                         ? getForkArgIndices(cb).start_routine_arg
                         : 2;
      if (const llvm::Value *arg = getCallArg(inst, idx)) {
        arg = arg->stripPointerCasts();
        if (const auto *ce = llvm::dyn_cast<llvm::ConstantExpr>(arg)) {
          if (ce->isCast())
            arg = ce->getOperand(0)->stripPointerCasts();
        }
        return arg;
      }
      return nullptr;
    }

    if (!hasMappedAPIEntry(cb))
      return nullptr;
    unsigned idx = getForkArgIndices(cb).start_routine_arg;
    if (const llvm::Value *arg = getCallArg(inst, idx))
      return arg->stripPointerCasts();
    return nullptr;
  }
  inline const llvm::Value *getForkedFun(const llvm::CallBase *cb) const {
    return getForkedFun(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// Return the user-argument passed to the start routine (configurable;
  /// default 3)
  inline const llvm::Value *
  getActualParmAtForkSite(const llvm::Instruction *inst) const {
    if (!isForkLike(inst))
      return nullptr;
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (cb && isKernelOperation(cb, kernel::OperationKind::KTHREAD_START)) {
      const llvm::CallBase *create = getKernelThreadCreateCall(inst);
      return create && create->arg_size() > 1 ? create->getArgOperand(1)
                                               : nullptr;
    }
    if ((cb && getType(cb) == TD_CUDA_KERNEL_LAUNCH) ||
        (getCallee(inst) && CUDAModel::isLegacyKernelConfiguration(
                                getCallee(inst)->getName())) ||
        isCppThreadLikeFork(getCallee(inst)) ||
        !hasMappedAPIEntry(cb))
      return nullptr;
    unsigned idx = getForkArgIndices(cb).arg_arg;
    return getCallArg(inst, idx);
  }
  inline const llvm::Value *
  getActualParmAtForkSite(const llvm::CallBase *cb) const {
    return getActualParmAtForkSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// Return thread payload arguments that may be observed by the spawned task.
  /// For pthread-style forks this is the user data argument; for std::thread-
  /// style forks these are the arguments after the callable.
  inline llvm::SmallVector<const llvm::Value *, 4>
  getForkPayloadArgs(const llvm::Instruction *inst) const {
    llvm::SmallVector<const llvm::Value *, 4> payload_args;
    if (!isForkLike(inst)) {
      return payload_args;
    }

    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const llvm::Function *callee = getCallee(inst);
    if (!cb || !callee) {
      return payload_args;
    }

    if (CUDAModel::isLegacyKernelConfiguration(callee->getName())) {
      if (const llvm::CallBase *kernel_call = getNextCallInBlock(inst)) {
        for (unsigned idx = 0; idx < kernel_call->arg_size(); ++idx) {
          payload_args.push_back(kernel_call->getArgOperand(idx));
        }
      }
      return payload_args;
    }

    if (getType(cb) == TD_CUDA_KERNEL_LAUNCH) {
      const CUDAModel::KernelLaunchLayout layout =
          CUDAModel::getKernelLaunchLayout(getCalledAPIName(cb));
      if (const llvm::Value *args = getCallArg(inst, layout.payload_arg))
        payload_args.push_back(args);
      return payload_args;
    }

    if (getType(cb) == TD_CUDA_MULTI_DEVICE_LAUNCH) {
      // Kernels and parameter arrays are nested in an array of launch records.
      // Keep the operation fork-like but do not invent direct operand layouts.
      return payload_args;
    }

    if (getRuntimeLibrary(cb) == RuntimeLibrary::OpenMP &&
        hasSemanticTag(cb, "fork")) {
      const ForkArgIndices layout = getForkArgIndices(cb);
      const unsigned begin = layout.variadic_payload
                                 ? layout.payload_begin
                                 : layout.arg_arg;
      if (begin != ForkArgIndices::NoArgument) {
        for (unsigned idx = begin; idx < cb->arg_size(); ++idx)
          payload_args.push_back(cb->getArgOperand(idx));
      }
      return payload_args;
    }

    if (!isCppThreadLikeFork(callee)) {
      if (const llvm::Value *arg = getActualParmAtForkSite(inst)) {
        payload_args.push_back(arg);
      }
      return payload_args;
    }

    const unsigned search_start = getCppForkCallableSearchStart(inst);
    unsigned payload_start = cb->arg_size();
    for (unsigned idx = search_start; idx < cb->arg_size(); ++idx) {
      const llvm::Value *arg = cb->getArgOperand(idx);
      const llvm::Value *stripped = arg ? arg->stripPointerCasts() : nullptr;
      if (llvm::isa<llvm::Function>(stripped)) {
        payload_start = idx + 1;
        break;
      }
    }

    if (payload_start == cb->arg_size()) {
      // No direct function operand was recovered. Conservatively treat every
      // remaining operand as potentially captured by a spawned task. This keeps
      // escape/TLS reasoning sound for functor/lambda launches.
      payload_start = search_start;
    }

    for (unsigned idx = payload_start; idx < cb->arg_size(); ++idx) {
      payload_args.push_back(cb->getArgOperand(idx));
    }

    return payload_args;
  }

  inline llvm::SmallVector<const llvm::Value *, 4>
  getForkPayloadArgs(const llvm::CallBase *cb) const {
    return getForkPayloadArgs(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Get the task function (i.e., the 5th parameter) of the hare_parallel_for
  /// call
  //@{
  inline const llvm::Value *
  getTaskFuncAtHareParForSite(const llvm::Instruction *inst) const {
    const llvm::Value *arg = getCallArg(inst, 4);
    return arg ? arg->stripPointerCasts() : nullptr;
  }

  inline const llvm::Value *
  getTaskFuncAtHareParForSite(const llvm::CallBase *cb) const {
    return getTaskFuncAtHareParForSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Get the task data (i.e., the 6th parameter) of the hare_parallel_for call
  //@{
  inline const llvm::Value *
  getTaskDataAtHareParForSite(const llvm::Instruction *inst) const {
    assert(isHareParFor(inst) && "not a hare_parallel_for function!");
    return getCallArg(inst, 5);
  }
  inline const llvm::Value *
  getTaskDataAtHareParForSite(const llvm::CallBase *cb) const {
    return getTaskDataAtHareParForSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call wait for a worker thread
  //@{
  inline bool isJoinLike(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    return t == TD_JOIN || t == TD_JTHREAD_JOIN || t == TD_CUDA_DEVICE_SYNC;
  }
  inline bool isJoinLike(const llvm::CallBase *cb) const {
    return isJoinLike(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  inline bool isTDJoin(const llvm::Instruction *inst) const {
    return isJoinLike(inst);
  }
  inline bool isTDJoin(const llvm::CallBase *cb) const {
    return isJoinLike(cb);
  }
  //@}

  /// Return arguments/attributes of pthread_join
  //@{
  /// Return the thread handle argument (configurable via thread.spec; default
  /// 0)
  inline const llvm::Value *
  getJoinedThread(const llvm::Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (cb && getType(cb) == TD_CUDA_DEVICE_SYNC) {
      return nullptr;
    }
    unsigned idx = getJoinArgIndices(cb).thread_arg;
    const llvm::Value *join = getCallField(inst, idx);
    if (!join)
      return nullptr;
    const llvm::Value *stripped = join->stripPointerCasts();
    if (llvm::isa<llvm::Argument>(stripped) ||
        llvm::isa<llvm::AllocaInst>(stripped))
      return stripped;
    if (stripped->getType()->isPointerTy())
      return stripped;
    // Preserve the SSA value for phi/select/scalar forwarding so callers can
    // trace it further instead of giving up immediately.
    return stripped;
  }
  inline const llvm::Value *getJoinedThread(const llvm::CallBase *cb) const {
    return getJoinedThread(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  /// Return the return-value argument (configurable; default 1)
  inline const llvm::Value *
  getRetParmAtJoinedSite(const llvm::Instruction *inst) const {
    assert(isTDJoin(inst) && "not a thread join function!");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (cb && getType(cb) == TD_CUDA_DEVICE_SYNC) {
      return nullptr;
    }
    const llvm::Function *callee = getCallee(inst);
    if (callee && (CppThreadingModel::isJoin(callee->getName()) ||
                   CppThreadingModel::isJthreadJoin(callee->getName())))
      return nullptr;
    unsigned idx = getJoinArgIndices(cb).ret_arg;
    return getCallField(inst, idx);
  }
  inline const llvm::Value *
  getRetParmAtJoinedSite(const llvm::CallBase *cb) const {
    return getRetParmAtJoinedSite(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call exits/terminate a thread
  //@{
  inline bool isTDExit(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    return cb && getType(cb) == TD_EXIT;
  }

  inline bool isTDExit(const llvm::CallBase *cb) const {
    return getType(cb) == TD_EXIT;
  }
  //@}

  /// Return true if this call acquire a lock (mutex or rwlock read/write)
  //@{
  inline bool isTDAcquire(const llvm::Instruction *inst) const {
    return isExclusiveLockAcquire(inst) || isSharedLockAcquire(inst);
  }

  inline bool isTDAcquire(const llvm::CallBase *cb) const {
    return isExclusiveLockAcquire(cb) || isSharedLockAcquire(cb);
  }
  //@}

  /// Return true if this call release a lock
  //@{
  inline bool isTDRelease(const llvm::Instruction *inst) const {
    return isExclusiveLockRelease(inst) || isSharedLockRelease(inst);
  }

  inline bool isTDRelease(const llvm::CallBase *cb) const {
    return isExclusiveLockRelease(cb) || isSharedLockRelease(cb);
  }
  //@}

  /// Return true if this call is a try-lock (e.g., pthread_mutex_trylock)
  //@{
  inline bool isTryLock(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    const APIDescription description = cb ? describe(cb) : describe(callee);
    const kernel::LinuxKernelAPISemantics *kernel_semantics =
        cb ? lookupKernelSemantics(getCalledAPIName(cb)) : nullptr;
    return t == TD_TRY_ACQUIRE || t == TD_SEMAPHORE_TRY_ACQUIRE ||
           t == TD_CPP_LOCK_TRY ||
           llvm::is_contained(description.traits, std::string("try-lock")) ||
           (kernel_semantics != nullptr &&
            kernel_semantics->operation == kernel::OperationKind::LOCK_TRY) ||
           (callee &&
            CppThreadingModel::isTryToLockConstructor(callee->getName())) ||
           (callee && (callee->getName() == "pthread_rwlock_tryrdlock" ||
                       callee->getName() == "pthread_rwlock_trywrlock")) ||
           // Linux kernel try-locks
           t == TD_KERNEL_SPIN_TRYLOCK || t == TD_KERNEL_MUTEX_TRYLOCK;
  }
  inline bool isTryLock(const llvm::CallBase *cb) const {
    return isTryLock(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isConditionalLockAcquire(const llvm::Instruction *inst) const {
    if (!inst)
      return false;
    if (isTryLock(inst))
      return true;
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb)
      return false;
    const APIDescription description = cb ? describe(cb) : describe(callee);
    if (description.conditional_acquire)
      return true;
    const kernel::LinuxKernelAPISemantics *kernel_semantics =
        lookupKernelSemantics(getCalledAPIName(cb));
    if (kernel_semantics != nullptr &&
        kernel_semantics->operation == kernel::OperationKind::LOCK_TRY) {
      return true;
    }
    const llvm::StringRef name = callee->getName();
    return name == "pthread_mutex_timedlock" ||
           name == "pthread_mutex_clocklock" ||
           name == "pthread_rwlock_timedrdlock" ||
           name == "pthread_rwlock_clockrdlock" ||
           name == "pthread_rwlock_timedwrlock" ||
           name == "pthread_rwlock_clockwrlock";
  }

  inline bool isConditionalLockAcquire(const llvm::CallBase *cb) const {
    return isConditionalLockAcquire(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call acquires a read lock (rwlock_rdlock)
  //@{
  inline bool isReadLockAcquire(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : getType(getCallee(inst));
    return t == TD_RWLOCK_RDLOCK || t == TD_SHARED_RDLOCK ||
           (t == TD_CPP_LOCK_TRY && getCallee(inst) &&
            (CppThreadingModel::isSharedLockTryAcquire(
                 getCallee(inst)->getName()) ||
             CppThreadingModel::isSharedTimedLockTryAcquire(
                 getCallee(inst)->getName()) ||
             CppThreadingModel::isSharedLockTryLock(
                 getCallee(inst)->getName()))) ||
           // Linux kernel read locks
           t == TD_KERNEL_READ_LOCK || t == TD_KERNEL_DOWN_READ;
  }
  inline bool isReadLockAcquire(const llvm::CallBase *cb) const {
    return isReadLockAcquire(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call acquires a write lock (rwlock_wrlock)
  //@{
  inline bool isWriteLockAcquire(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : getType(getCallee(inst));
    return t == TD_RWLOCK_WRLOCK || t == TD_SHARED_WRLOCK ||
           (t == TD_CPP_LOCK_TRY && getCallee(inst) &&
            !CppThreadingModel::isSharedLockTryAcquire(
                getCallee(inst)->getName()) &&
            !CppThreadingModel::isSharedTimedLockTryAcquire(
                getCallee(inst)->getName())) ||
           // Linux kernel write locks
           t == TD_KERNEL_WRITE_LOCK || t == TD_KERNEL_DOWN_WRITE;
  }
  inline bool isWriteLockAcquire(const llvm::CallBase *cb) const {
    return isWriteLockAcquire(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return lock value
  //@{
  /// First argument of pthread_mutex_lock/pthread_mutex_unlock/pthread_rwlock_*
  inline const llvm::Value *getLockVal(const llvm::Instruction *inst) const {
    if (!inst || (!isTDAcquire(inst) && !isTDRelease(inst)))
      return nullptr;
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb || cb->arg_size() == 0)
      return nullptr;

    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = getType(cb);
    switch (t) {
    case TD_LOCK_GUARD_CTOR:
    case TD_UNIQUE_LOCK_CTOR:
    case TD_SCOPED_LOCK_CTOR:
    case TD_SHARED_LOCK_CTOR:
      return cb->arg_size() > 1 ? cb->getArgOperand(1) : nullptr;
    case TD_CPP_LOCK_TRY:
      if (getCallee(inst) &&
          (CppThreadingModel::isUniqueLockTryLock(
               getCallee(inst)->getName()) ||
           CppThreadingModel::isSharedLockTryLock(
               getCallee(inst)->getName())))
        return getCppWrapperUnderlyingLock(inst);
      return cb->getArgOperand(0);
    case TD_SEMAPHORE_ACQUIRE:
    case TD_SEMAPHORE_RELEASE:
    case TD_SEMAPHORE_TRY_ACQUIRE:
    case TD_SHARED_RDLOCK:
    case TD_SHARED_WRLOCK:
    case TD_SHARED_UNLOCK:
      if (getCallee(inst) &&
          (CppThreadingModel::isSharedLockLock(getCallee(inst)->getName()) ||
           CppThreadingModel::isSharedLockUnlock(getCallee(inst)->getName())))
        return getCppWrapperUnderlyingLock(inst);
      return cb->getArgOperand(0);
    case TD_OMP_ORDERED_START:
    case TD_OMP_ORDERED_END:
    case TD_ACQUIRE:
    case TD_TRY_ACQUIRE:
    case TD_RWLOCK_RDLOCK:
    case TD_RWLOCK_WRLOCK:
    case TD_RELEASE:
    case TD_KERNEL_SPIN_LOCK:
    case TD_KERNEL_SPIN_TRYLOCK:
    case TD_KERNEL_MUTEX_LOCK:
    case TD_KERNEL_MUTEX_TRYLOCK:
    case TD_KERNEL_DOWN:
    case TD_KERNEL_READ_LOCK:
    case TD_KERNEL_WRITE_LOCK:
    case TD_KERNEL_DOWN_READ:
    case TD_KERNEL_DOWN_WRITE:
    case TD_KERNEL_SPIN_UNLOCK:
    case TD_KERNEL_MUTEX_UNLOCK:
    case TD_KERNEL_UP:
    case TD_KERNEL_READ_UNLOCK:
    case TD_KERNEL_WRITE_UNLOCK:
    case TD_KERNEL_UP_READ:
    case TD_KERNEL_UP_WRITE:
      return cb->getArgOperand(0);
    case TD_LOCK_GUARD_DTOR:
    case TD_UNIQUE_LOCK_DTOR:
    case TD_UNIQUE_LOCK_LOCK:
    case TD_UNIQUE_LOCK_UNLOCK:
    case TD_SCOPED_LOCK_DTOR:
    case TD_SHARED_LOCK_DTOR:
      return nullptr;
    default:
      return cb->getArgOperand(0);
    }
  }
  inline const llvm::Value *getLockVal(const llvm::CallBase *cb) const {
    return getLockVal(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return the lock/synchronization identity used by analyses.
  /// This differs from getLockVal() for wrapper-style locks and OpenMP
  /// critical regions where the raw lock operand is not the stable analysis
  /// key.
  inline const llvm::Value *
  getAnalysisLockIdentity(const llvm::Instruction *inst) const {
    if (!inst) {
      return nullptr;
    }
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb || cb->arg_size() == 0) {
      return nullptr;
    }

    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = getType(cb);
    switch (t) {
    case TD_LOCK_GUARD_CTOR:
    case TD_LOCK_GUARD_DTOR:
    case TD_UNIQUE_LOCK_CTOR:
    case TD_UNIQUE_LOCK_DTOR:
    case TD_UNIQUE_LOCK_LOCK:
    case TD_UNIQUE_LOCK_UNLOCK:
    case TD_SCOPED_LOCK_CTOR:
    case TD_SCOPED_LOCK_DTOR:
    case TD_SHARED_LOCK_CTOR:
    case TD_SHARED_LOCK_DTOR:
    case TD_CPP_LOCK_MOVE_CTOR:
    case TD_CPP_LOCK_MOVE_ASSIGN:
    case TD_CPP_LOCK_SWAP:
    case TD_CPP_LOCK_RELEASE:
      if (const llvm::Value *underlying = getCppWrapperUnderlyingLock(inst)) {
        return underlying;
      }
      if (t == TD_SCOPED_LOCK_CTOR || t == TD_SCOPED_LOCK_DTOR ||
          t == TD_CPP_LOCK_MOVE_CTOR || t == TD_CPP_LOCK_MOVE_ASSIGN ||
          t == TD_CPP_LOCK_SWAP || t == TD_CPP_LOCK_RELEASE)
        return nullptr;
      return cb->getArgOperand(0)->stripPointerCasts();
    case TD_SHARED_RDLOCK:
    case TD_SHARED_UNLOCK:
      if (callee &&
          (CppThreadingModel::isSharedLockLock(callee->getName()) ||
           CppThreadingModel::isSharedLockUnlock(callee->getName())))
        return getCppWrapperUnderlyingLock(inst);
      return getLockVal(inst);
    case TD_CPP_LOCK_TRY:
      if (getCallee(inst) &&
          (CppThreadingModel::isUniqueLockTryLock(
               getCallee(inst)->getName()) ||
           CppThreadingModel::isSharedLockTryLock(
               getCallee(inst)->getName())))
        return getCppWrapperUnderlyingLock(inst);
      return cb->getArgOperand(0)->stripPointerCasts();
    case TD_ACQUIRE:
    case TD_RELEASE:
    case TD_OMP_ORDERED_START:
    case TD_OMP_ORDERED_END:
      if (cb->arg_size() >= 3) {
        const llvm::Function *callee = getCallee(inst);
        if (callee && (hasSemanticTag(callee, "critical-enter") ||
                       hasSemanticTag(callee, "critical-exit"))) {
          return cb->getArgOperand(2)->stripPointerCasts();
        }
      }
      if (t == TD_OMP_ORDERED_START || t == TD_OMP_ORDERED_END) {
        return inst;
      }
      return getLockVal(inst);
    default:
      return getLockVal(inst);
    }
  }

  inline llvm::SmallVector<const llvm::Value *, 4>
  getAnalysisLockIdentities(const llvm::Instruction *inst) const {
    llvm::SmallVector<const llvm::Value *, 4> identities;
    if (!inst)
      return identities;
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE type = cb ? getType(cb) : getType(getCallee(inst));
    if (type == TD_SCOPED_LOCK_CTOR || type == TD_SCOPED_LOCK_DTOR ||
        type == TD_CPP_LOCK_MOVE_CTOR ||
        type == TD_CPP_LOCK_MOVE_ASSIGN || type == TD_CPP_LOCK_SWAP ||
        type == TD_CPP_LOCK_RELEASE) {
      identities = getCppWrapperUnderlyingLocks(inst);
      return identities;
    }
    if (const llvm::Value *identity = getAnalysisLockIdentity(inst))
      identities.push_back(identity);
    return identities;
  }

  inline llvm::SmallVector<const llvm::Value *, 4>
  getAnalysisLockIdentities(const llvm::CallBase *cb) const {
    return getAnalysisLockIdentities(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool
  cppWrapperDestructorReleases(const llvm::Instruction *inst) const {
    return cppWrapperDestructorDefinitelyReleases(inst);
  }

  inline const llvm::Value *
  getAnalysisLockIdentity(const llvm::CallBase *cb) const {
    return getAnalysisLockIdentity(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline LockSemanticInfo
  getLockSemanticInfo(const llvm::Instruction *inst) const {
    LockSemanticInfo info;
    if (!inst) {
      return info;
    }

    info.identities = getAnalysisLockIdentities(inst);
    info.identity = info.identities.size() == 1 ? info.identities.front() : nullptr;
    if (info.identities.empty()) {
      return info;
    }

    const llvm::Function *callee = getCallee(inst);
    const llvm::StringRef name = callee ? callee->getName() : llvm::StringRef();
    if (CppThreadingModel::isDeferLockConstructor(name)) {
      info.ownership = LockOwnershipEffect::Deferred;
      return info;
    }
    if (CppThreadingModel::isTryToLockConstructor(name)) {
      info.ownership = LockOwnershipEffect::Try;
      info.is_try = true;
    } else if (CppThreadingModel::isAdoptLockConstructor(name)) {
      info.ownership = LockOwnershipEffect::Adopt;
      return info;
    }

    if (isTDRelease(inst)) {
      info.kind = LockSemanticKind::Release;
      info.ownership = LockOwnershipEffect::Release;
      return info;
    }

    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE type = cb ? getType(cb) : getType(callee);
    if ((type == TD_UNIQUE_LOCK_DTOR || type == TD_SHARED_LOCK_DTOR) &&
        !cppWrapperDestructorDefinitelyReleases(inst)) {
      info.kind = LockSemanticKind::Release;
      info.ownership = LockOwnershipEffect::Release;
      info.conditional = true;
      return info;
    }

    info.is_try = isTryLock(inst);
    info.conditional = isConditionalLockAcquire(inst);
    if (info.conditional) {
      const TD_TYPE type = getType(callee);
      const llvm::CallBase *cb = getLLVMCallSite(inst);
      const APIDescription description = cb ? describe(cb) : describe(callee);
      const kernel::LinuxKernelAPISemantics *kernel_semantics =
          cb ? lookupKernelSemantics(getCalledAPIName(cb)) : nullptr;
      if (description.success != TryLockSuccess::Unknown) {
        info.try_success = description.success;
      } else if (kernel_semantics != nullptr &&
                 kernel_semantics->success ==
                     kernel::ConditionalSuccess::ZERO) {
        info.try_success = TryLockSuccess::Zero;
      } else if (kernel_semantics != nullptr &&
                 kernel_semantics->success ==
                     kernel::ConditionalSuccess::NONZERO) {
        info.try_success = TryLockSuccess::NonZero;
      } else if (callee && callee->getName().startswith("pthread_")) {
        info.try_success = TryLockSuccess::Zero;
      } else if (type == TD_TRY_ACQUIRE ||
                 type == TD_SEMAPHORE_TRY_ACQUIRE ||
                 type == TD_CPP_LOCK_TRY || type == TD_KERNEL_SPIN_TRYLOCK ||
                 type == TD_KERNEL_MUTEX_TRYLOCK ||
                 (callee &&
                  CppThreadingModel::isTryToLockConstructor(
                      callee->getName()))) {
        info.try_success = TryLockSuccess::NonZero;
      }
    }
    if (isReadLockAcquire(inst)) {
      info.kind = LockSemanticKind::Shared;
    } else if (isWriteLockAcquire(inst) || isTDAcquire(inst)) {
      info.kind = LockSemanticKind::Exclusive;
    }
    return info;
  }

  inline LockSemanticInfo getLockSemanticInfo(const llvm::CallBase *cb) const {
    return getLockSemanticInfo(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// Return true if this call waits for a barrier
  //@{
  inline bool isTDBarWait(const llvm::Instruction *inst) const {
    return isBarrierWaitLike(inst);
  }

  inline bool isTDBarWait(const llvm::CallBase *cb) const {
    return isBarrierWaitLike(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return barrier value
  //@{
  /// First argument of barrier-style synchronization operations.
  inline const llvm::Value *getBarrierVal(const llvm::Instruction *inst) const {
    assert(isBarrierLike(inst) && "not a barrier-like function");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb)
      return nullptr;
    TD_TYPE t = getType(cb);
    if (t == TD_OMP_REDUCE_START) {
      return inst;
    }
    const llvm::Function *callee = getCallee(inst);
    if (t == TD_BAR_WAIT && callee &&
        getRuntimeLibrary(callee) == RuntimeLibrary::OpenMP) {
      // __kmpc_barrier commonly reuses/nulls its ident_t* operand. The static
      // synchronization identity is the barrier site itself, not that metadata.
      return inst;
    }
    if (cb->arg_size() < 1) {
      return nullptr;
    }
    return cb->getArgOperand(0);
  }
  inline const llvm::Value *getBarrierVal(const llvm::CallBase *cb) const {
    return getBarrierVal(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return true if this call waits on a condition variable
  //@{
  inline bool isTDCondWait(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : TD_DUMMY;
    return t == TD_COND_WAIT;
  }

  inline bool isTDCondWait(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(cb);
    return t == TD_COND_WAIT;
  }
  //@}

  /// Return true if this call signals a condition variable
  //@{
  inline bool isTDCondSignal(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : TD_DUMMY;
    return t == TD_COND_SIGNAL;
  }

  inline bool isTDCondSignal(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(cb);
    return t == TD_COND_SIGNAL;
  }
  //@}

  /// Return true if this call broadcasts a condition variable
  //@{
  inline bool isTDCondBroadcast(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : TD_DUMMY;
    return t == TD_COND_BROADCAST;
  }

  inline bool isTDCondBroadcast(const llvm::CallBase *cb) const {
    TD_TYPE t = getType(cb);
    return t == TD_COND_BROADCAST;
  }
  //@}

  /// Return condition variable value
  //@{
  /// First argument of pthread_cond_wait/signal/broadcast
  inline const llvm::Value *getCondVal(const llvm::Instruction *inst) const {
    assert((isTDCondWait(inst) || isTDCondSignal(inst) ||
            isTDCondBroadcast(inst)) &&
           "not a condition variable function");
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb || cb->arg_size() < 1)
      return nullptr;
    return cb->getArgOperand(0);
  }
  inline const llvm::Value *getCondVal(const llvm::CallBase *cb) const {
    return getCondVal(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  /// Return mutex value associated with condition wait
  //@{
  /// Second argument of pthread_cond_wait
  inline const llvm::Value *getCondMutex(const llvm::Instruction *inst) const {
    assert(isTDCondWait(inst) && "not a condition wait function");
    return getConditionVariableWaitMutex(inst);
  }
  inline const llvm::Value *getCondMutex(const llvm::CallBase *cb) const {
    return getCondMutex(llvm::dyn_cast<llvm::Instruction>(cb));
  }
  //@}

  void performAPIStat(llvm::Module *m);
  void statInit(llvm::StringMap<u32_t> &tdAPIStatMap);

  // ========================================================================
  // Convenience group predicates (avoid enumerating all enum values at call
  // sites)
  // ========================================================================

  inline LockSemantics
  describeLockSemantics(const llvm::Instruction *inst) const {
    LockSemantics semantics;
    if (!inst) {
      return semantics;
    }

    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const TD_TYPE t = cb ? getType(cb) : getType(getCallee(inst));
    const bool shared_acquire = isReadLockAcquire(inst);
    const bool shared_release = isSharedLockRelease(inst);
    const bool exclusive_acquire = isExclusiveLockAcquire(inst);
    const bool exclusive_release = isExclusiveLockRelease(inst);

    semantics.is_acquire = exclusive_acquire || shared_acquire;
    semantics.is_release = exclusive_release || shared_release;
    semantics.is_lock_api = semantics.is_acquire || semantics.is_release;
    semantics.is_try = isTryLock(inst);
    semantics.is_semaphore = isSemaphoreOp(inst);
    semantics.is_binary_semaphore = isBinarySemaphoreOp(inst);

    if (shared_acquire || shared_release) {
      semantics.mode = LockMode::Shared;
    } else if (exclusive_acquire || exclusive_release) {
      semantics.mode = LockMode::Exclusive;
    }

    return semantics;
  }

  inline LockSemantics describeLockSemantics(const llvm::CallBase *cb) const {
    return describeLockSemantics(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isExclusiveLockAcquire(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    const bool binary_semaphore =
        cb && hasTrait(cb, "binary-semaphore") && isSemaphoreOp(inst);
    return ((t == TD_ACQUIRE || t == TD_TRY_ACQUIRE) &&
            !(cb && hasTrait(cb, "semaphore"))) ||
           t == TD_RWLOCK_WRLOCK || t == TD_SHARED_WRLOCK ||
           t == TD_OMP_ORDERED_START ||
           (t == TD_LOCK_GUARD_CTOR &&
            !(callee && CppThreadingModel::isAdoptLockConstructor(
                            callee->getName()))) ||
           (t == TD_UNIQUE_LOCK_CTOR &&
            !(callee && (CppThreadingModel::isDeferLockConstructor(
                             callee->getName()) ||
                         CppThreadingModel::isAdoptLockConstructor(
                             callee->getName())))) ||
           t == TD_UNIQUE_LOCK_LOCK ||
           t == TD_SCOPED_LOCK_CTOR ||
           (t == TD_CPP_LOCK_TRY && !isReadLockAcquire(inst)) ||
           ((t == TD_SEMAPHORE_ACQUIRE ||
             t == TD_SEMAPHORE_TRY_ACQUIRE ||
             ((t == TD_ACQUIRE || t == TD_TRY_ACQUIRE) && cb &&
              hasTrait(cb, "semaphore"))) &&
            binary_semaphore) ||
           t == TD_KERNEL_SPIN_LOCK || t == TD_KERNEL_SPIN_TRYLOCK ||
           t == TD_KERNEL_MUTEX_LOCK || t == TD_KERNEL_MUTEX_TRYLOCK ||
           t == TD_KERNEL_DOWN || t == TD_KERNEL_WRITE_LOCK ||
           t == TD_KERNEL_DOWN_WRITE;
  }

  inline bool isExclusiveLockAcquire(const llvm::CallBase *cb) const {
    return isExclusiveLockAcquire(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isExclusiveLockRelease(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    const bool binary_semaphore =
        cb && hasTrait(cb, "binary-semaphore") && isSemaphoreOp(inst);
    const bool shared_mutex_exclusive_unlock =
        t == TD_SHARED_UNLOCK && callee &&
        (CppThreadingModel::isSharedLockExclusiveRelease(callee->getName()) ||
         CppThreadingModel::isSharedTimedLockExclusiveRelease(
             callee->getName()));
    return (t == TD_RELEASE && !(cb && hasTrait(cb, "semaphore"))) ||
           shared_mutex_exclusive_unlock ||
           t == TD_OMP_ORDERED_END || t == TD_LOCK_GUARD_DTOR ||
           (t == TD_UNIQUE_LOCK_DTOR &&
            cppWrapperDestructorDefinitelyReleases(inst)) ||
           t == TD_UNIQUE_LOCK_UNLOCK ||
           t == TD_SCOPED_LOCK_DTOR ||
           ((t == TD_SEMAPHORE_RELEASE ||
             (t == TD_RELEASE && cb && hasTrait(cb, "semaphore"))) &&
            binary_semaphore) ||
           t == TD_KERNEL_SPIN_UNLOCK || t == TD_KERNEL_MUTEX_UNLOCK ||
           t == TD_KERNEL_UP || t == TD_KERNEL_WRITE_UNLOCK ||
           t == TD_KERNEL_UP_WRITE;
  }

  inline bool isExclusiveLockRelease(const llvm::CallBase *cb) const {
    return isExclusiveLockRelease(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isSharedLockAcquire(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    return t == TD_RWLOCK_RDLOCK || t == TD_SHARED_RDLOCK ||
           (t == TD_SHARED_LOCK_CTOR &&
            !(callee && (CppThreadingModel::isDeferLockConstructor(
                             callee->getName()) ||
                         CppThreadingModel::isAdoptLockConstructor(
                             callee->getName())))) ||
           t == TD_KERNEL_READ_LOCK ||
           t == TD_KERNEL_DOWN_READ;
  }

  inline bool isSharedLockAcquire(const llvm::CallBase *cb) const {
    return isSharedLockAcquire(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isSharedLockRelease(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    const llvm::Function *callee = getCallee(inst);
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    const bool exclusive_unlock =
        callee &&
        (CppThreadingModel::isSharedLockExclusiveRelease(callee->getName()) ||
         CppThreadingModel::isSharedTimedLockExclusiveRelease(
             callee->getName()));
    return (t == TD_SHARED_UNLOCK && !exclusive_unlock) ||
           (t == TD_SHARED_LOCK_DTOR &&
            cppWrapperDestructorDefinitelyReleases(inst)) ||
           t == TD_KERNEL_READ_UNLOCK || t == TD_KERNEL_UP_READ;
  }

  inline bool isSharedLockRelease(const llvm::CallBase *cb) const {
    return isSharedLockRelease(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  /// True for any C++20 barrier/latch/semaphore synchronization operation.
  inline bool isBarrierOp(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : getType(getCallee(inst));
    return t == TD_BAR_WAIT || t == TD_BAR_INIT || t == TD_LATCH_COUNT_DOWN ||
           t == TD_LATCH_WAIT || t == TD_LATCH_ARRIVE_WAIT ||
           t == TD_BARRIER_ARRIVE_WAIT || t == TD_BARRIER_ARRIVE ||
           t == TD_CUDA_BARRIER || t == TD_CUDA_WARP_BARRIER ||
           t == TD_BARRIER_WAIT_CPP20;
  }

  /// True for any semaphore acquire/release operation.
  inline bool isSemaphoreOp(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb) {
      return false;
    }
    TD_TYPE t = getType(cb);
    if (isSemaphoreType(t)) {
      return true;
    }
    return hasTrait(cb, "semaphore") &&
           (t == TD_ACQUIRE || t == TD_TRY_ACQUIRE || t == TD_RELEASE);
  }

  inline bool isSemaphoreOp(const llvm::CallBase *cb) const {
    return isSemaphoreOp(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isBinarySemaphoreOp(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (!cb || !isSemaphoreOp(inst)) {
      return false;
    }
    if (hasTrait(cb, "binary-semaphore")) {
      return true;
    }
    return callee->getName().contains("binary_semaphore");
  }

  inline bool isBinarySemaphoreOp(const llvm::CallBase *cb) const {
    return isBinarySemaphoreOp(llvm::dyn_cast<llvm::Instruction>(cb));
  }

  inline bool isLatchLike(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : TD_DUMMY;
    return t == TD_LATCH_COUNT_DOWN || t == TD_LATCH_WAIT ||
           t == TD_LATCH_ARRIVE_WAIT;
  }

  inline bool isBarrierWaitLike(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    if (cb && hasTrait(cb, "barrier-wait-like")) {
      return true;
    }
    TD_TYPE t = cb ? getType(cb) : getType(callee);
    return t == TD_BAR_WAIT || t == TD_LATCH_ARRIVE_WAIT ||
           t == TD_BARRIER_ARRIVE_WAIT || t == TD_BARRIER_WAIT_CPP20 ||
           t == TD_CUDA_BARRIER || t == TD_CUDA_WARP_BARRIER;
  }

  inline bool isBarrierLike(const llvm::Instruction *inst) const {
    return isBarrierOp(inst) || isBlockingMPIBarrier(inst) || isOMPTaskOp(inst);
  }

  inline bool isLockLike(const llvm::Instruction *inst) const {
    return isTDAcquire(inst) || isTDRelease(inst);
  }

  inline bool isBlockingMPIBarrier(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    if (!callee)
      return false;
    return hasTrait(callee, "mpi-barrier-blocking");
  }

  inline bool isNonBlockingMPIBarrier(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    if (!callee)
      return false;
    return hasTrait(callee, "mpi-barrier-nonblocking");
  }

  inline bool isBlockingMPICollective(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    if (!callee)
      return false;
    return hasTrait(callee, "mpi-collective-blocking");
  }

  inline bool isNonBlockingMPICollective(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    if (!callee)
      return false;
    return hasTrait(callee, "mpi-collective-nonblocking");
  }

  /// True for any atomic synchronization operation (future/promise/call_once).
  inline bool isAtomicSyncOp(const llvm::Instruction *inst) const {
    const llvm::CallBase *cb = getLLVMCallSite(inst);
    TD_TYPE t = cb ? getType(cb) : TD_DUMMY;
    return t == TD_CALL_ONCE || t == TD_FUTURE_GET || t == TD_FUTURE_WAIT ||
           t == TD_PROMISE_SET || t == TD_ASYNC || t == TD_ATOMIC_WAIT ||
           t == TD_ATOMIC_NOTIFY_ONE || t == TD_ATOMIC_NOTIFY_ALL;
  }

  /// True for any MPI collective or barrier (synchronization point).
  inline bool isMPICollective(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && hasTrait(callee, "mpi-collective");
  }

  inline bool isMPICommunicatorManagement(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && semanticTagStartsWith(callee, "comm-");
  }

  inline bool isMPIRequestManagement(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && (semanticTagStartsWith(callee, "request-") ||
                      semanticTagStartsWith(callee, "persistent-") ||
                      semanticTagStartsWith(callee, "start"));
  }

  inline bool isPersistentMPIRequestInit(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && hasTrait(callee, "mpi-persistent-init");
  }

  inline bool isPersistentMPIRequestStart(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && hasTrait(callee, "mpi-request-start");
  }

  /// True for any OpenMP task-related operation.
  inline bool isOMPTaskOp(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && hasTrait(callee, "omp-task-op");
  }

  inline bool isOMPTargetOp(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && hasTrait(callee, "omp-target-op");
  }

  inline bool isOMPTargetDataOp(const llvm::Instruction *inst) const {
    const llvm::Function *callee = getCallee(inst);
    return callee && hasTrait(callee, "omp-target-data-op");
  }

  /// Convert a TD_TYPE to a human-readable string (for diagnostics).
  static const char *tdTypeToString(TD_TYPE t);

  /// Parse a TD_TYPE name (used by thread.spec and tests).
  static TD_TYPE stringToType(llvm::StringRef name);
};

#endif // THREADAPI_H
