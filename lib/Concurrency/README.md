# Analysis of Concurrent Programs

## Directory layout

Headers live under `include/Concurrency/`, sources under `lib/Concurrency/`, grouped by subdirectory:

| Subdirectory | Contents |
|--------------|----------|
| **Utils/** | Thread APIs, flow graphs, vector clocks, atomics, and RAII lock support |
| **MHP/** | May-happen-in-parallel and happens-before analyses |
| **LockSet/** | Intra- and interprocedural lock-set analysis |
| **Memory/** | Escape and thread-sharing analyses |
| **Thread/** | Thread creation-tree analysis |
| **JoinTarget/** | Join-target resolution |
| **ValueFlow/** | Thread-aware sparse value-flow refinement |
| **OpenMP/** | OpenMP semantics, task graphs, and data sharing |
| **CUDA/** | CUDA semantics, memory model, and kernel protocol analysis |
| **MPI/** | MPI process, collective, protocol, rank, and RMA analyses |
| **LinuxKernel/** | Linux kernel concurrency and lifetime analyses |

Each source subdirectory owns a `CMakeLists.txt` containing its source manifest.
The parent directory aggregates those manifests into the stable `Concurrency`
library target, which preserves compatibility for existing consumers.

Include paths use these subdirs, e.g. `Concurrency/Utils/ThreadAPI.h`, `Concurrency/MHP/MHPAnalysis.h`, `Concurrency/MPI/MPIAnalysis.h`.

## Utilities

- **ThreadAPI**: Provides an API for identifying and categorizing thread-related operations (fork, join, lock, unlock, etc.) in LLVM IR.
- **ThreadFlowGraph**: Implements a graph representation of thread synchronization operations and their ordering relationships.
- **FBVClock**: Implements a fast bit-vector clock system for tracking happens-before relationships in concurrent programs.
- **BVClock**: Provides a bit-vector clock implementation for vector clocks used in concurrency analysis.

## Analyses 
- **LockSetAnalysis**: Performs may/must lock set analysis to track which locks are held at each program point.
- **HappensBeforeAnalysis**: Determines happens-before relationships between instructions from TFG program order plus synchronizes-with edges.
- **MHPAnalysis**: Implements pure may-happen-in-parallel overlap analysis to determine which instructions can execute concurrently.
- **StaticVectorClockMHP**: Implements a static vector clock-based approach for MHP analysis.
- **StaticThreadSharingAnalysis**: Analyzes which memory locations are shared between threads using static analysis.
- **EscapeAnalysis**: Determines which values escape their thread-local scope and become shared between threads.
- **JoinTargetAnalysis**: For each pthread_join, computes the set of pthread_create calls that may be joined (join's arg0 may alias fork's arg0). Supports unambiguous-join reasoning for MHP refinement.


## Data-race checker (Ultimate borrows)

- **Sync object exclusion**: Accesses to lock/cond/barrier objects (first arg of pthread_mutex_*, pthread_cond_*, pthread_barrier_*) are excluded from race checking.
- **Data-race report schema**: ConcurrencyBugReport supports optional **DataRaceInfo** (access path, read/write, conflicting pair) for witness/SARIF.


## Modern C++ Support (C++11/14/17/20)

### Threading Primitives
- **std::thread**: Full support for thread creation, joining, and detaching
- **std::jthread** (C++20): Automatic joining thread support

### Mutex and Locking
- **std::mutex**: lock, try_lock, unlock
- **std::recursive_mutex**: Recursive mutex operations
- **std::shared_mutex** (C++17): Shared/exclusive locking (lock_shared, lock, unlock_shared, unlock)
- **std::shared_timed_mutex** (C++17): Timed shared locking

### RAII Lock Wrappers
- **std::lock_guard**: RAII exclusive lock wrapper
- **std::unique_lock**: Movable RAII lock with manual lock/unlock support
- **std::scoped_lock** (C++17): Multi-lock RAII wrapper for deadlock-free locking
- **std::shared_lock** (C++17): RAII shared lock wrapper

### Synchronization Primitives
- **std::condition_variable**: wait, notify_one, notify_all
- **std::call_once** / **std::once_flag**: Recognized; definite HB still needs winner tracking
- **std::future** / **std::promise**: Task-based synchronization (get, wait, set_value, set_exception)
- **std::async**: Asynchronous task creation
- **std::latch** (C++20): Recognized; definite HB still needs phase/count tracking
- **std::barrier** (C++20): Recognized; definite HB still needs phase tracking
- **std::counting_semaphore** / **std::binary_semaphore** (C++20): Semaphore primitives

### Atomic Operations
- **Memory-order recognition**: relaxed, acquire, release, acq_rel, seq_cst
- **Atomic operations**: load, store, exchange, compare_exchange, fetch_add, etc.
- **Fence instructions**: atomic_thread_fence with all memory orders
- **Synchronizes-with relationship tracking**: witness-based fence ordering; plain release/acquire remains deferred unless proven
- **Release sequences**: recognized, but no synthetic global ordering is emitted without witnesses

## OpenMP Support (3.0 - 5.x)

### Basic Constructs
- **#pragma omp parallel**: Parallel regions (__kmpc_fork_call)
- **#pragma omp barrier**: Explicit barriers
- **#pragma omp critical**: Critical sections
- **#pragma omp master**: Master-only regions
- **#pragma omp single**: Single-thread execution
- **#pragma omp ordered**: Ordered execution

### Worksharing
- **#pragma omp for**: Parallel loops (static, dynamic, guided scheduling)
- **#pragma omp sections**: Independent code sections
- **#pragma omp single**: Single execution with implicit barrier

### Task-Based Parallelism (3.0+)
- **#pragma omp task**: Explicit task creation (__kmpc_omp_task)
- **#pragma omp taskwait**: Wait for child tasks (__kmpc_omp_taskwait)
- **#pragma omp taskyield**: Yield to other tasks (__kmpc_omp_taskyield)
- **#pragma omp taskgroup**: Task group synchronization
- **#pragma omp taskloop** (4.5+): Loop-based task creation
- **Task dependencies** (4.0+): depend clauses for task ordering

### Advanced Features
- **Target offloading** (4.0+): Device execution support
- **Atomic operations**: __kmpc_atomic_start/end
- **Flush**: Memory fence operations
- **Cancellation** (4.0+): Cancellation points and cancel constructs
- **OpenMP locks**: omp_set_lock, omp_unset_lock, nested locks

## MPI Support (MPI-1, MPI-2, MPI-3)

### Process Management
- **MPI_Init** / **MPI_Init_thread**: Initialize MPI environment
- **MPI_Finalize**: Finalize MPI environment
- **MPI_Comm_rank** / **MPI_Comm_size**: Query process rank and communicator size

### Point-to-Point Communication (Blocking)
- **MPI_Send** / **MPI_Ssend** / **MPI_Bsend** / **MPI_Rsend**: Blocking send operations
- **MPI_Recv**: Blocking receive
- **MPI_Sendrecv** / **MPI_Sendrecv_replace**: Combined send/receive
- **MPI_Probe**: Probe for incoming messages

### Point-to-Point Communication (Non-blocking)
- **MPI_Isend** / **MPI_Issend** / **MPI_Ibsend** / **MPI_Irsend**: Non-blocking send operations
- **MPI_Irecv**: Non-blocking receive
- **MPI_Iprobe**: Non-blocking probe
- **MPI_Wait** / **MPI_Waitall** / **MPI_Waitany** / **MPI_Waitsome**: Wait for completion
- **MPI_Test** / **MPI_Testall** / **MPI_Testany** / **MPI_Testsome**: Test for completion

### Collective Communication
- **MPI_Barrier** / **MPI_Ibarrier**: Synchronization barrier
- **MPI_Bcast** / **MPI_Ibcast**: Broadcast
- **MPI_Scatter** / **MPI_Scatterv** / **MPI_Iscatter**: Scatter data
- **MPI_Gather** / **MPI_Gatherv** / **MPI_Igather**: Gather data
- **MPI_Allgather** / **MPI_Allgatherv** / **MPI_Iallgather**: All-gather
- **MPI_Alltoall** / **MPI_Alltoallv** / **MPI_Alltoallw**: All-to-all exchange
- **MPI_Reduce** / **MPI_Ireduce**: Reduce operation
- **MPI_Allreduce** / **MPI_Iallreduce**: All-reduce operation
- **MPI_Reduce_scatter** / **MPI_Reduce_scatter_block**: Reduce-scatter
- **MPI_Scan** / **MPI_Exscan** / **MPI_Iscan**: Scan operations

### One-Sided Communication (RMA - MPI-2/3)
- **MPI_Win_create** / **MPI_Win_allocate** / **MPI_Win_create_dynamic**: Window creation
- **MPI_Win_free**: Window deallocation
- **MPI_Put** / **MPI_Rput**: Remote write operations
- **MPI_Get** / **MPI_Rget**: Remote read operations
- **MPI_Accumulate** / **MPI_Get_accumulate** / **MPI_Fetch_and_op** / **MPI_Compare_and_swap**: Atomic RMA operations

### RMA Synchronization
- **Active Target (Fence)**: MPI_Win_fence
- **Passive Target (Lock/Unlock)**: MPI_Win_lock, MPI_Win_unlock, MPI_Win_lock_all, MPI_Win_unlock_all
- **Completion**: MPI_Win_flush, MPI_Win_flush_all, MPI_Win_flush_local, MPI_Win_sync
- **General Purpose (PSCW)**: MPI_Win_post, MPI_Win_start, MPI_Win_complete, MPI_Win_wait, MPI_Win_test

### Communicator Management
- **MPI_Comm_dup** / **MPI_Comm_idup**: Duplicate communicator
- **MPI_Comm_split** / **MPI_Comm_split_type**: Split communicator
- **MPI_Comm_create** / **MPI_Comm_create_group**: Create communicator
- **MPI_Comm_free**: Free communicator

## Analysis Capabilities

### Lock Set Analysis
- **May-lockset**: Over-approximation of locks that might be held
- **Must-lockset**: Under-approximation of locks that must be held
- **Read/write lock tracking**: Separate tracking for shared_mutex read vs write locks
- **RAII lock tracking**: Automatic tracking of lock_guard, unique_lock, scoped_lock lifetimes
- **Semaphore tracking**: C++20 counting_semaphore and binary_semaphore
- **Lock ordering**: Deadlock detection via lock order inversion analysis
- **Interprocedural**: Function summaries for context-sensitive lock analysis

### Happens-Before Analysis
- **Atomic synchronization**: witness-based fence synchronization; plain release/acquire edges are deferred by default
- **Fence synchronization**: modeled when a same-location atomic witness chain is provable
- **Condition variables**: wait/signal/broadcast are region boundaries; definite signal→wait HB edges are not synthesized yet
- **Future/Promise synchronization**: promise.set_value() → future.get() edges
- **call_once synchronization**: Recognized, but no definite HB edges are emitted yet
- **Latch synchronization**: Recognized, but no definite HB edges are emitted yet
- **Barrier synchronization**: Recognized, but no definite HB edges are emitted yet
- **Release sequences**: RMW chains properly modeled for happens-before

### Data Race Detection
- **Atomic awareness**: Uses enhanced memory ordering semantics
- **RAII lock protection**: Understands implicit acquire/release from RAII locks
- **Shared vs exclusive locks**: Distinguishes reader-writer lock semantics

### Lock Mismatch Detection
- **RAII lock patterns**: Detects mismatched lock_guard, unique_lock usage
- **unique_lock manual operations**: Tracks manual lock()/unlock() calls
- **Shared lock patterns**: Validates shared_lock and shared_mutex usage

### MPI Analysis (NEW)
- **Process-level concurrency**: Models SPMD execution pattern
- **Point-to-point tracking**: Tracks blocking and non-blocking send/recv operations
- **Deadlock detection**: Identifies circular send/recv dependencies
- **Collective mismatch**: Detects incompatible collective operations
- **Orphaned requests**: Finds non-blocking operations without matching wait
- **RMA race detection**: Detects data races in one-sided communication
- **RMA synchronization**: Validates fence/lock/PSCW synchronization epochs
- **Window leak detection**: Identifies RMA windows not properly freed

## Recent Fixes (2026-02)

### Correctness
- **Barrier HB cycle fix** (`MHPAnalysis.cpp`): `handleBarrier` previously added bidirectional
  HB edges between barrier arrivals, creating cycles in the HB graph that caused
  `hasHappenBeforeRelation` to return `true` for unrelated instructions.  Fixed to add
  one-directional edges from each earlier arrival to the current one; the symmetric direction
  is added naturally when the earlier arrivals are processed.
- **Seq-cst total order** (`MHPAnalysis.cpp`): `computeSeqCstTotalOrder` previously added edges
  between *all* seq-cst stores and loads regardless of location, which could suppress real races
  on unrelated variables.  Fixed to only add edges between operations on potentially aliasing
  locations (alias analysis already skips non-aliasing pairs).  RMW–RMW pairs are now also
  ordered correctly.
- **CppThreading shared_mutex exclusion** (`CppThreading.h`): `isAcquire` / `isRelease` /
  `isTryAcquire` previously matched `shared_mutex` operations, causing them to be classified
  as plain exclusive locks instead of the more precise `TD_SHARED_RDLOCK` / `TD_SHARED_WRLOCK`
  types.  Fixed by adding `!funcName.contains("shared")` guards.
- **OpenMP null-callee guard** (`OpenMP.h`): `isFork(CallBase*)` did not guard against a null
  callee (indirect call), causing a null-pointer dereference.  Fixed.

### Performance
- **MHP pairs storage** (`MHPAnalysis.h/.cpp`): `m_mhp_pairs` and `m_atomic_hb_pairs` changed
  from `std::set<std::pair<...>>` (O(log N) per lookup) to
  `std::unordered_set<InstPair, InstPairHash>` (O(1) average).  Pairs are stored in canonical
  pointer-sorted order so `isPrecomputedMHP` requires only a single hash probe instead of two.

### Code Quality
- **OpenMP anonymous namespace** (`OpenMP.h`): `matchesAny` was defined inside an anonymous
  namespace in a header, causing ODR violations when the header was included in multiple
  translation units.  Changed to a plain `inline` function.

## Limitations

- **std::memory_order_consume**: Recognized but treated as acquire (per C++ standard recommendation)
- **Weak atomics precision**: Relaxed atomics tracked but no value-flow analysis
- **MPI derived datatypes**: Custom MPI datatypes not fully analyzed for size/alignment
- **MPI intercommunicator operations**: Focus is on intracommunicators; intercommunicator collectives have limited support
- **OpenMP task dependencies**: Basic dependency graph structure implemented; full symbolic dependency analysis ongoing
- **MPI rank analysis**: Symbolic rank tracking implemented; path-sensitive refinement ongoing

## Extension Guidelines

The concurrency analysis framework is designed for easy extensibility to support additional concurrency libraries.

### Adding a New Concurrency Library

1. **Create Language Model** (`include/Concurrency/Utils/LanguageModel/YourLib.h`):
   - Define inline pattern-matching functions for library operations
   - Group by operation type (fork, join, lock, barrier, etc.)
   - Follow naming convention: `isOperationName(const llvm::StringRef& funcName)`

2. **Extend ThreadAPI Enum** (`include/Concurrency/Utils/ThreadAPI.h`):
   - Add new `TD_TYPE` enum values for library-specific operations
   - Group related operations together
   - Document each enum value

3. **Update ThreadAPI Recognition** (`lib/Concurrency/Utils/ThreadAPI.cpp`):
   - Include your language model header
   - Add recognition logic in `ThreadAPI::getType()`
   - Check operations in order of specificity

4. **Create Library-Specific Analysis** (optional):
   - For complex semantics (like MPI's SPMD model), create dedicated analysis
   - Place in `include/Concurrency/YourLib/` and `lib/Concurrency/YourLib/`

5. **Update Build System**:
   - Add source files to `lib/Concurrency/CMakeLists.txt`

6. **Document Support**:
   - Update `lib/Concurrency/README.md` with supported operations
   - Add analysis capabilities

### Example: Adding CUDA Support

```cpp
// include/Concurrency/Utils/LanguageModel/CUDA.h
namespace CUDAModel {
  inline bool isKernelLaunch(const llvm::StringRef& funcName) {
    return funcName.contains("cudaLaunchKernel");
  }
  inline bool isDeviceSynchronize(const llvm::StringRef& funcName) {
    return funcName.equals("cudaDeviceSynchronize");
  }
  inline bool isSyncthreads(const llvm::StringRef& funcName) {
    return funcName.equals("llvm.nvvm.barrier0");
  }
}

// Add to ThreadAPI.h enum:
TD_CUDA_KERNEL_LAUNCH,
TD_CUDA_DEVICE_SYNC,
TD_CUDA_SYNCTHREADS,

// Add to ThreadAPI.cpp:
if (CUDAModel::isKernelLaunch(name)) return TD_CUDA_KERNEL_LAUNCH;
```

### Design Principles

- **Separation of Concerns**: Each library gets its own `LanguageModel` namespace
- **Extensibility**: Use dynamic dispatch via enum for easy extension
- **Config-driven**: Support `config/thread.spec` for user-defined APIs
- **Semantic Mapping**: Map library calls to semantic operations (fork/join/barrier)
- **Hierarchical Recognition**: Exact match → config → language models
