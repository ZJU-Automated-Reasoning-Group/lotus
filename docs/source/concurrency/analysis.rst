Concurrency Analysis
====================

Thread-aware analyses for concurrent programs.

**Headers**: ``include/Concurrency``

**Implementation**: ``lib/Concurrency``

Overview
--------

The concurrency analysis module provides the shared analysis layer used by Lotus
concurrency checking. It centers on thread creation, synchronization operations,
memory accesses, and communication structure that can be recovered from LLVM IR.

The module supports multiple concurrency models:

- **POSIX threads (pthreads)**: standard pthread APIs
- **Modern C++ concurrency**: ``std::thread``, mutexes, condition variables,
  futures/promises, atomics, and several C++20 primitives that are recognized
- **OpenMP**: parallel regions, barriers, tasks, and lock operations
- **MPI**: a separate SPMD communication analysis in ``Concurrency/MPI/``
- **Custom APIs**: configurable through ``config/thread.spec`` and
  ``ThreadAPI``

**Key capabilities**:

- May-Happen-in-Parallel (MHP) analysis for shared-memory overlap reasoning
- Lock-set analysis for race and deadlock checking
- Happens-before computation from thread-flow and synchronizes-with edges
- Escape and thread-sharing analyses for shared-memory filtering
- Join-target reasoning for refining ``pthread_join`` effects
- Thread flow graph construction and thread API classification utilities

Directory layout
----------------

The source tree is grouped by subdirectory under ``include/Concurrency/`` and
``lib/Concurrency/``:

- ``Utils/``: ``ThreadAPI``, ``ThreadFlowGraph``, vector-clock utilities,
  RAII lock tracking, and language models for C++, OpenMP, MPI, and Linux kernel
  APIs
- ``MHP/``: ``MHPAnalysis``, ``StaticVectorClockMHP``, and
  ``HappensBeforeAnalysis``
- ``LockSet/``: ``LockSetAnalysis``
- ``Memory/``: ``EscapeAnalysis`` and ``StaticThreadSharingAnalysis``
- ``JoinTarget/``: ``JoinTargetAnalysis``
- ``MPI/``: ``MPIAnalysis`` and its process, collective, rank, and RMA analyses
- ``CUDA/``: ``CUDAAnalysis``, ``CUDAFunctionSummary``, ``CUDASemantics``, and
  ``PTXAnalyzer`` for GPU thread/block hierarchy reasoning
- ``LinuxKernel/``: ``LinuxKernelAnalysis``, ``LinuxKernelLockAnalysis``, and
  ``LinuxKernelRCUAnalysis`` for kernel concurrency primitives
- ``ValueFlow/``: thread-aware sparse value-flow refinement
  (``ThreadAwareSVFG``, ``SparseValueFlowRefinement``,
  ``WholeProgramSparseRefinement``, ``FSMPTA``, and ``MultiStageSlicer``)

``Concurrency/MPI/`` models MPI communication in the SPMD setting. It complements
the shared-memory analyses above, but it is not another thread library layered on
top of ``MHPAnalysis``.

Main Components
---------------

MHPAnalysis
~~~~~~~~~~~

**File**: ``MHP/MHPAnalysis.cpp``, ``MHP/MHPAnalysis.h``

The core May-Happen-in-Parallel analysis determines which pairs of program
statements may execute concurrently in a multithreaded program.

**Analysis Process**:

1. **Thread Flow Graph Construction**: Builds a graph representation of thread
   creation, synchronization operations, and their ordering relationships

2. **Thread Region Analysis**: Identifies regions of code that execute in
   different threads

3. **Happens-Before Analysis**: Computes happens-before relationships using
   fork-join semantics, locks, condition variables, and barriers

4. **MHP Pair Computation**: Precomputes pairs of instructions that may execute
   in parallel

**Key Features**:

- Fork-join analysis for thread creation and termination
- Lock-based synchronization analysis (optional integration with LockSetAnalysis)
- Condition variable analysis (wait/signal/broadcast)
- Barrier synchronization support
- Multi-instance thread tracking (e.g., threads created in loops)
- Efficient query interface with precomputed results

**Query Interface**:

- ``mayHappenInParallel(I1, I2)`` – Check if two instructions may execute concurrently
- ``mustBeSequential(I1, I2)`` – Check if two instructions must execute sequentially
- ``getParallelInstructions(inst)`` – Get all instructions that may run in parallel
- ``isPrecomputedMHP(I1, I2)`` – Check if a pair is in the precomputed MHP set

LockSetAnalysis
~~~~~~~~~~~~~~~

**File**: ``LockSet/LockSetAnalysis.cpp``, ``LockSet/LockSetAnalysis.h``

Computes the sets of locks that may or must be held at each program point.
Essential for data race detection, deadlock detection, and precise MHP analysis.

**Analysis Modes**:

1. **May-Lockset Analysis** (over-approximation):
   - Computes the set of locks that *may* be held at each program point
   - Uses union operation at control flow merge points
   - Used for conservative data race detection

2. **Must-Lockset Analysis** (under-approximation):
   - Computes the set of locks that *must* be held at each program point
   - Uses intersection operation at control flow merge points
   - Used for proving absence of data races

**Supported Operations**:

- ``pthread_mutex_lock/unlock``
- ``pthread_rwlock_rdlock/wrlock/unlock``
- ``sem_wait/post``
- ``pthread_mutex_trylock`` (try-lock operations)
- Reentrant lock handling
- Lock aliasing support

**Features**:

- Intraprocedural lock set computation using dataflow analysis
- Interprocedural lock set propagation across function calls
- Lock ordering tracking for deadlock detection
- Integration with alias analysis for precise lock identification

**Query Interface**:

- ``getMayLockSetAt(inst)`` – Get may-lockset at an instruction
- ``getMustLockSetAt(inst)`` – Get must-lockset at an instruction
- ``isLockHeldAt(lock, inst)`` – Check if a specific lock is held

JoinTargetAnalysis
~~~~~~~~~~~~~~~~~~

**File**: ``JoinTarget/JoinTargetAnalysis.cpp``,
``JoinTarget/JoinTargetAnalysis.h``

Computes which ``pthread_create`` sites may match a given ``pthread_join`` by
reasoning about the joined thread handle. This is used to refine thread
termination effects beyond a simple name-based match.

**Use cases**:

- Refining join reasoning when multiple thread handles may alias
- Supporting more precise MHP pruning around thread termination
- Providing a dedicated analysis for the ``JoinTarget/`` subdirectory that now
  exists in the source tree

ThreadAPI
~~~~~~~~~

**File**: ``Utils/ThreadAPI.cpp``, ``Utils/ThreadAPI.h``

Provides a unified interface for identifying and categorizing thread-related
operations in LLVM IR. Acts as an abstraction layer over different threading
models.

**Supported Operations**:

- **Thread Management**: Fork, join, detach, exit, cancel
- **Synchronization**: Lock acquire/release, try-lock
- **Condition Variables**: Wait, signal, broadcast
- **Barriers**: Init, wait
- **Mutex Management**: Init, destroy

**Threading Model Support**:

- **POSIX threads**: pthread_create, pthread_join, pthread_mutex_lock, etc.
- **C++11 threads**: std::thread, std::mutex, std::condition_variable
- **OpenMP**: Parallel regions, barriers, critical sections
- **Custom APIs**: Configurable via configuration files

**Key Methods**:

- ``isTDFork(inst)`` – Check if instruction creates a thread
- ``isTDJoin(inst)`` – Check if instruction joins a thread
- ``isTDAcquire(inst)`` – Check if instruction acquires a lock
- ``isTDRelease(inst)`` – Check if instruction releases a lock
- ``getForkedThread(inst)`` – Get the pthread_t value for a fork
- ``getForkedFun(inst)`` – Get the function executed by a thread

HappensBeforeAnalysis
~~~~~~~~~~~~~~~~~~~~~

**File**: ``MHP/HappensBeforeAnalysis.cpp``, ``MHP/HappensBeforeAnalysis.h``

Determines happens-before relationships between instructions using MHP analysis
and synchronization operations. The happens-before relation is fundamental for
reasoning about memory ordering and data races.

**Happens-Before Sources**:

1. **Program Order**: Sequential execution within a thread
2. **Fork-Join**: Thread creation and join edges
3. **Locks**: lock and unlock synchronization
4. **Condition Variables**: wait, signal, and broadcast are recognized as thread
   flow boundaries
5. **Barriers and atomics**: barriers, fences, and selected atomic patterns

**Features**:

- Graph reachability over thread-flow and explicit synchronizes-with edges
- Efficient query interface
- Integration with MHP analysis

Some synchronization constructs are recognized more precisely than they are
fully proved. For example, ``std::call_once``, ``std::latch``,
``std::barrier``, and condition-variable signaling are modeled conservatively,
and definite happens-before edges are still limited for some of them.

ThreadFlowGraph
~~~~~~~~~~~~~~~

**File**: ``Utils/ThreadFlowGraph.cpp``, ``Utils/ThreadFlowGraph.h``

Implements a graph representation of thread synchronization operations and their
ordering relationships. Used as an intermediate representation for MHP and
happens-before analyses.

**Node Types**:

- Thread fork/join nodes
- Lock acquire/release nodes
- Condition variable nodes
- Barrier nodes
- Thread exit nodes

**Features**:

- Graph construction from thread API calls
- Edge representation of ordering relationships
- Support for visualization and debugging

EscapeAnalysis
~~~~~~~~~~~~~~

**File**: ``Memory/EscapeAnalysis.cpp``, ``Memory/EscapeAnalysis.h``

Determines which values escape their thread-local scope and become shared between
threads. Essential for identifying which memory locations may be accessed by
multiple threads.

**Analysis Process**:

1. **Thread-Local Identification**: Identifies values that are thread-local
2. **Escape Detection**: Tracks when values are passed to other threads
3. **Shared Memory Identification**: Marks memory locations as shared

**Use Cases**:

- Data race detection (only shared memory can have races)
- Memory safety analysis in concurrent contexts
- Optimizing thread-local storage

StaticVectorClockMHP
~~~~~~~~~~~~~~~~~~~~

**File**: ``MHP/StaticVectorClockMHP.cpp``, ``MHP/StaticVectorClockMHP.h``

Implements a static vector clock-based approach for MHP analysis. Uses vector
clocks to efficiently track happens-before relationships and compute MHP pairs.

**Features**:

- Map-based static vector clocks over abstract logic-clock elements
- Efficient MHP computation
- Scalable to large programs

StaticThreadSharingAnalysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**File**: ``Memory/StaticThreadSharingAnalysis.cpp``, ``Memory/StaticThreadSharingAnalysis.h``

Analyzes which memory locations are shared between threads using static analysis.
Combines escape analysis with thread flow information to identify shared memory.

**Features**:

- Static identification of shared memory
- Integration with alias analysis
- Support for complex pointer structures

Thread-Aware Sparse Value-Flow Refinement
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Files**: ``ValueFlow/ThreadAwareSVFG.cpp``, ``ValueFlow/ThreadAwareSVFG.h``,
``ValueFlow/SparseValueFlowRefinement.cpp``,
``ValueFlow/SparseValueFlowRefinement.h``,
``ValueFlow/WholeProgramSparseRefinement.cpp``,
``ValueFlow/WholeProgramSparseRefinement.h``, ``ValueFlow/FSMPTA.cpp``,
``ValueFlow/FSMPTA.h``, ``ValueFlow/MultiStageSlicer.cpp``,
``ValueFlow/MultiStageSlicer.h``

The ``ValueFlow/`` subdirectory implements the optional whole-program refinement
used by the concurrency checker: a thread-aware sparse value-flow analysis that
refines data-race alias pairs with a flow-sensitive points-to solve over the
SVFG. It was introduced by the thread-aware sparse SVFG refinement commit
(``d13d1543``). The general solver lives in
``Alias/InclusionBased/FlowSensitive/`` and is composed with the thread-aware
SVFG by ``FSMPTA``; the concurrency layer does not duplicate it.

**ThreadAwareSVFGBuilder** (``ThreadAwareSVFG.h``): adds guarded Store-to-Load
and symmetric Store-to-Store ``ThreadMHPIndirectVF`` edges for accesses that may
run in parallel. The base SVFG remains owned by the caller and can be restored
with ``clear()``. A ``FilteredSVFGView`` can restrict construction to an induced
subgraph without copying graph nodes. Fork and join memory flow is connected
through dedicated passes (``connectForkFlow``, ``connectJoinFlow``).

**SparseValueFlowRefinement** (``SparseValueFlowRefinement.h``): a lightweight
concurrency-specific diagnostic refinement, introduced as
``SparseFlowSensitivePTA`` in the original commit and later renamed. It solves
pointer-value flow over the SVFG and its thread-interference overlay, attaching
memory values to sparse MemorySSA definitions rather than to every LLVM
instruction. It is not the alias-analysis oracle and cannot suppress race
candidates on its own. Queries include ``pointsTo``, ``memoryValue``,
``hasCompletePointsTo``, ``accessTargets``, and ``mayAliasAccesses`` (which
returns ``nullopt`` when refinement is unavailable, otherwise whether two
accesses may still address a common abstract object). Mutable and hash-consed
points-to set backends are supported.

**FlowSensitivePTA** (``Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h``):
the general sparse flow-sensitive inclusion-based pointer analysis. It owns the
top-level points-to sets and per-node MemorySSA ``IN``/``OUT`` state.

**FSMPTA** (``FSMPTA.h``): composes ``FlowSensitivePTA`` with the thread-aware
SVFG and an optional sliced solve scope.

**WholeProgramSparseRefinement** (``WholeProgramSparseRefinement.h``): ownership
wrapper for a complete analysis run. It owns the ICFG, SVFG, overlay, and
solver, and supports two modes: ``WholeProgram`` and ``MultiStageSlicing``.
Configuration covers the memory-region partition strategy, the thread context
limit (default 2), and the points-to set backend.

**MultiStageSlicer** (``MultiStageSlicer.h``): implements the MSli pipeline:
candidate closure, synchronization/call expansion, and a bounded PTA closure.
The pre-analysis overlay is discarded; MHP and lock queries are evaluated again
while constructing the filtered main-phase overlay.

**ThreadCallGraph / ThreadCreationTree**
(``Concurrency/Thread/ThreadCreationTree.h``): rebuild context-bounded thread
instances for both the pre-analysis and sliced main phase. Forks in loops or
recursive contexts are marked multi-instance, and joins are resolved through
canonicalized thread handles.

**Thread-aware MemorySSA**: treats a fork as forward-only and a join as
return-only. Caller memory definitions and payloads flow into the start routine,
while joined ``FormalOut`` definitions flow to synthetic ``ActualOut`` nodes and
dominated post-join accesses.

**Memory region partition policies**: ``distinct`` preserves each exact
points-to set; ``intra-disjoint`` merges overlapping sets independently per
function; ``inter-disjoint`` merges overlapping sets over the whole module.
Freezing the partition before MemorySSA construction prevents overlapping
points-to sets from silently receiving unrelated SSA version streams.

**Checker integration**: ``ConcurrencyChecker`` owns a
``WholeProgramSparseRefinement`` and builds it when sparse flow-sensitive
refinement is enabled and data-race checking is active, recording statistics
such as interference edges and points-to facts. The original commit wired the
sparse solver into ``DataRaceChecker``, which consults ``mayAliasAccesses``
while deciding whether two accesses may share a location. Incomplete or
wildcard points-to results never suppress a race candidate; the checker only
removes a pair when complete sparse results prove its access-target sets
disjoint.

**Enabling**: the path is opt-in. Run:

.. code-block:: text

   lotus-check --engine=concur --checks=data-race \
     --concur.sparse-flow-sensitive input.bc

Multi-stage slicing is enabled with:

.. code-block:: text

   lotus-check --engine=concur --checks=data-race --concur.msli \
     --concur.memory-partition=inter-disjoint \
     --concur.points-to-sets=hash-consed input.bc

The sparse analysis and hash-consed backend are both opt-in; mutable ordered
sets remain the default.

See also :doc:`../ir/svfg` for the underlying sparse value-flow graph and
:doc:`../checker/concurrency` for the checker that consumes this refinement.

Usage
-----

**Basic MHP Analysis**:

.. code-block:: cpp

   #include <Concurrency/MHP/MHPAnalysis.h>

   llvm::Module &M = ...;
   mhp::MHPAnalysis mhp(M);
   mhp.analyze();

   const llvm::Instruction *I1 = ...;
   const llvm::Instruction *I2 = ...;

   if (mhp.mayHappenInParallel(I1, I2)) {
     // I1 and I2 may execute concurrently
   }

**LockSet Analysis**:

.. code-block:: cpp

   #include <Concurrency/LockSet/LockSetAnalysis.h>

   llvm::Module &M = ...;
   mhp::LockSetAnalysis lsa(M);
   lsa.analyze();

   const llvm::Instruction *inst = ...;
   mhp::LockSet mayLocks = lsa.getMayLockSetAt(inst);
   mhp::LockSet mustLocks = lsa.getMustLockSetAt(inst);

**Combined Analysis for Data Race Detection**:

.. code-block:: cpp

   #include <Concurrency/MHP/MHPAnalysis.h>
   #include <Concurrency/LockSet/LockSetAnalysis.h>
   #include <Concurrency/Memory/EscapeAnalysis.h>

   llvm::Module &M = ...;
   
   // Setup analyses
   mhp::MHPAnalysis mhp(M);
   mhp.enableLockSetAnalysis();
   mhp.analyze();
   
   mhp::EscapeAnalysis escape(M);
   escape.analyze();
   
   // Check for data races
   for (auto &I1 : instructions) {
     for (auto &I2 : instructions) {
       if (mhp.mayHappenInParallel(I1, I2) &&
           isMemoryAccess(I1) && isMemoryAccess(I2) &&
           mayAlias(I1, I2) &&
           (isWrite(I1) || isWrite(I2)) &&
           !isProtectedByCommonLock(I1, I2, lsa) &&
           escape.isShared(getMemoryLocation(I1))) {
         // Potential data race detected
       }
     }
   }

**ThreadAPI Usage**:

.. code-block:: cpp

   #include <Concurrency/Utils/ThreadAPI.h>

   ThreadAPI *api = ThreadAPI::getThreadAPI();
   
   for (auto &F : M) {
     for (auto &I : instructions(F)) {
       if (api->isTDFork(&I)) {
         const llvm::Value *thread = api->getForkedThread(&I);
         const llvm::Value *func = api->getForkedFun(&I);
         // Process thread creation
       }
     }
   }

Data race detection: when we report
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The data race checker reports one bug per **racy component** (a connected set of
conflicting accesses), not one per instruction pair. This reduces noise and
matches the “one report per logical race” idea used in tools like Goblint.

**When two accesses are considered a race**

A potential data race is reported only when all of the following hold:

1. **May happen in parallel (MHP)** – The two instructions may execute concurrently.
2. **At least one is a write** – Read-only pairs are not races.
3. **Not ordered by happens-before** – e.g. C11 synchronizes-with; if one is guaranteed to happen before the other, no race.
4. **May access the same location** – Alias analysis says the two memory locations may overlap.
5. **Not protected by a common lock** – Lock-set analysis shows no lock is held for both.
6. **Location is shared** – Escape analysis says the memory may be shared (e.g. escaped, or global).

**Same location**

“Same location” means: the two instructions’ memory locations **may alias** and the
location is **escaped** (or otherwise shared). Accesses to thread-local, non-escaped
storage are skipped. Accesses to **ignorable types** (sync objects, ``FILE``, atomics,
etc.) are never considered for races; see the checker’s ignorable-type list.

**One report per component**

Racy pairs are grouped into **components**: two accesses are in the same component if
they are connected by a chain of “may race” pairs. The checker emits **one report per
component**, with a representative pair and a step for each conflicting access. So
multiple overlapping pairs on the same variable yield a single report.

**Example**

Two threads both write to ``shared_counter`` without a lock: the checker finds one
racy component (both writes), and reports one data race with two steps (one per
write). If a third thread also wrote to ``shared_counter``, the same single report
would list all three accesses as steps.

**Typical use cases**:

- Data race detection in multithreaded programs
- Deadlock detection through lock ordering analysis
- Verifying thread safety of concurrent data structures
- Identifying potentially racy memory accesses
- Providing concurrency-aware information to higher-level analyses
- Security analysis of concurrent code

Limitations
----------

- **Atomic Operations**: Limited support for fine-grained memory ordering
  (std::memory_order_relaxed, acquire, release, etc.). The ThreadAPI maps
  atomics roughly to locks/barriers but doesn't handle the fine-grained
  "synchronizes-with" relationships of weak atomics.

- **Dynamic Thread Creation**: Analysis may be conservative for threads created
  in loops or conditionally, potentially leading to false positives in MHP
  analysis.

- **Interprocedural Precision**: Some analyses (e.g., LockSetAnalysis) support
  interprocedural analysis, but precision may be limited for complex call
  patterns or indirect calls.

- **Alias Analysis Dependency**: Accurate results require precise alias analysis.
  Imprecise alias analysis can lead to false positives in data race detection.

- **Language Model Coverage**: While supporting pthreads, C++11, and OpenMP,
  some less common threading APIs may not be recognized without configuration.

- **Real-Time Constraints**: The analysis does not model real-time scheduling
  constraints or priority-based execution ordering.
