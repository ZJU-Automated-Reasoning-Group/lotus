Concurrency Analysis
====================

Thread-aware analyses for concurrent programs.

**Headers**: ``include/Analysis/Concurrency``

**Implementation**: ``lib/Analysis/Concurrency``

Overview
--------

The Concurrency analysis module provides a comprehensive suite of static analyses
for reasoning about multithreaded programs. It enables detection of data races,
deadlocks, and other concurrency bugs by analyzing thread creation, synchronization
operations, and memory accesses.

The module supports multiple threading models:

- **POSIX threads (pthreads)**: Standard pthread API functions
- **C++11 threads**: std::thread, std::mutex, std::condition_variable
- **OpenMP**: Parallel regions, barriers, critical sections
- **Custom threading models**: Configurable via ThreadAPI

**Key capabilities**:

- May-Happen-in-Parallel (MHP) analysis to determine concurrent execution
- Lock-set analysis for data race detection
- Happens-before relationship computation
- Memory use-def analysis in concurrent contexts
- Escape analysis to identify shared memory locations
- Thread flow graph construction for synchronization modeling

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

MemUseDefAnalysis
~~~~~~~~~~~~~~~~~

**File**: ``Memory/MemUseDefAnalysis.cpp``, ``Memory/MemUseDefAnalysis.h``

Performs memory use-def analysis based on MemorySSA to track memory dependencies
in concurrent contexts. Identifies which memory definitions may reach each
memory use, accounting for concurrent execution.

**Analysis Process**:

1. **MemorySSA Construction**: Builds MemorySSA representation for each function
2. **Reaching Def Analysis**: Computes reaching definitions using dataflow analysis
3. **Interprocedural Propagation**: Propagates memory definitions across function calls
4. **Concurrent Context Integration**: Accounts for concurrent memory accesses

**Key Features**:

- MemorySSA-based analysis for precise memory modeling
- Interprocedural memory dataflow analysis
- Support for indirect calls and function pointers
- Integration with alias analysis for memory location identification

**Use Cases**:

- Identifying memory dependencies in concurrent code
- Supporting data race detection by tracking memory accesses
- Enabling precise analysis of shared memory operations

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
2. **Fork-Join**: Thread creation establishes happens-before
3. **Locks**: Lock acquire happens-before lock release
4. **Condition Variables**: Signal happens-before wait completion
5. **Barriers**: Barrier wait establishes happens-before

**Features**:

- Vector clock-based implementation (FBVClock, BVClock)
- Efficient query interface
- Integration with MHP analysis

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

- Fast bit-vector clock implementation (FBVClock)
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

Usage
-----

**Basic MHP Analysis**:

.. code-block:: cpp

   #include <Analysis/Concurrency/MHP/MHPAnalysis.h>

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

   #include <Analysis/Concurrency/LockSet/LockSetAnalysis.h>

   llvm::Module &M = ...;
   mhp::LockSetAnalysis lsa(M);
   lsa.analyze();

   const llvm::Instruction *inst = ...;
   mhp::LockSet mayLocks = lsa.getMayLockSetAt(inst);
   mhp::LockSet mustLocks = lsa.getMustLockSetAt(inst);

**Combined Analysis for Data Race Detection**:

.. code-block:: cpp

   #include <Analysis/Concurrency/MHP/MHPAnalysis.h>
   #include <Analysis/Concurrency/LockSet/LockSetAnalysis.h>
   #include <Analysis/Concurrency/Memory/EscapeAnalysis.h>

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

   #include <Analysis/Concurrency/Utils/ThreadAPI.h>

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

