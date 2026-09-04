Concurrency Bug Checker
========================

Static analysis tool for detecting concurrency bugs in multithreaded programs,
including shared-memory threading bugs plus dedicated OpenMP and MPI protocol
checks.

**Library Location**: ``lib/Checker/Concurrency/``

**Headers**: ``include/Checker/Concurrency/``

**Tool Location**: ``tools/checker/lotus-check-concur.cpp``

**Frontend**: ``lotus-check --engine=concur``

Overview
--------

The concurrency checker analyzes LLVM IR to detect thread safety issues using:

* **MHP (May Happen in Parallel) Analysis** – Determines which instructions can execute concurrently
* **Lock Set Analysis** – Tracks which locks protect shared memory accesses
* **Happens-Before Analysis** – Builds synchronization relationships between threads
* **OpenMP Task Analysis** – Tracks task creation, taskgroup/taskwait boundaries, and ``depend`` relations
* **MPI Communication Analysis** – Tracks point-to-point operations, collectives, requests, and RMA synchronization

All detected bugs are reported through the centralized ``BugReportMgr`` system, enabling unified JSON and SARIF output.

Components
----------

**ConcurrencyChecker** (``ConcurrencyChecker.cpp``, ``ConcurrencyChecker.h``):

* Main coordinator that orchestrates all concurrency checks
* Manages statistics and analysis configuration
* Integrates with ``BugReportMgr`` for centralized reporting

**DataRaceChecker** (``DataRaceChecker.cpp``, ``DataRaceChecker.h``):

* Detects data races using MHP analysis
* Identifies concurrent memory accesses without proper synchronization
* Reports bug type: ``BUG_DATARACE`` (CWE-362, HIGH importance, ERROR classification)

**DeadlockChecker** (``DeadlockChecker.cpp``, ``DeadlockChecker.h``):

* Detects potential deadlocks using lock set analysis
* Identifies circular lock dependencies between threads
* Analyzes lock acquisition order to find cycles

**AtomicityChecker** (``AtomicityChecker.cpp``, ``AtomicityChecker.h``):

* Detects atomicity violations
* Identifies non-atomic sequences of operations that should be atomic
* Checks for interleaving of critical sections

**OpenMPChecker** (``OpenMPChecker.cpp``, ``OpenMPChecker.h``):

* Dedicated checker for OpenMP-specific bug patterns
* Reports unbalanced taskgroup regions
* Reports unbalanced atomic runtime regions
* Warns about partial task synchronization through ``__kmpc_omp_wait_deps``

**MPIChecker** (``MPIChecker.cpp``, ``MPIChecker.h``):

* Dedicated checker for MPI-specific protocol and synchronization bugs
* Reports orphaned non-blocking requests
* Reports simple blocking communication deadlocks
* Reports collective mismatches and conditional collectives
* Reports unsynchronized RMA operations, RMA races, and leaked windows

Usage
-----

**Basic Usage**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc

**Enable Specific Checks**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --checks=data-race
   ./build/bin/lotus-check --engine=concur input.bc --checks=deadlock
   ./build/bin/lotus-check --engine=concur input.bc --checks=atomicity
   ./build/bin/lotus-check --engine=concur input.bc --checks=openmp
   ./build/bin/lotus-check --engine=concur input.bc --checks=mpi

**Select Checks with a Comma-Separated List**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --checks=data-race,deadlock,openmp
   ./build/bin/lotus-check --engine=concur input.bc --checks=mpi

**Enable Multiple Checks**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --checks=data-race,deadlock,atomicity

**Generate JSON Report**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --report-json=report.json

**Generate SARIF Report**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --report-sarif=report.sarif

Command-Line Options
--------------------

* ``--checks=<id[,id...]>`` – Select ``data-race``, ``deadlock``, ``atomicity``,
  ``condvar``, ``lock-mismatch``, ``openmp``, ``mpi``, or ``cuda``. Omitting
  this option, or passing ``--checks=all``, enables all checks.
* ``--concur.mode=analysis`` – Run analyses and dump facts without emitting bug reports
* ``--concur.output=<file>`` – Write analysis facts to a file in analysis mode
* ``--concur.output-format=<text|json>`` – Select the analysis-fact format
* ``--report-json=<file>`` – Output JSON report to file
* ``--report-sarif=<file>`` – Output SARIF report to file
* ``--report-min-score=<n>`` – Minimum confidence score for reporting (0-100)

Scope
-----

``concur`` is the dedicated engine for shared-memory, OpenMP, and MPI
concurrency errors.  Its ``--checks`` option selects only concurrency checker
families; it does not enable memory-safety, numerical, or taint engines.  See
:ref:`Choosing a Checker <choosing-a-checker>` for the complete checker guide.

Bug Types
---------

**Data Races** (``BUG_DATARACE``):

* **Description**: Concurrent access to shared memory without proper synchronization
* **CWE**: CWE-362
* **Importance**: HIGH
* **Classification**: ERROR
* **Detection**: MHP analysis identifies instructions that may execute in parallel and access the same memory location without locks

**Deadlocks**:

* **Description**: Circular lock dependencies between threads that can cause threads to wait indefinitely
* **Detection**: Lock set analysis identifies cycles in lock acquisition order
* **Analysis**: Tracks lock acquisition sequences across threads

**Atomicity Violations**:

* **Description**: Non-atomic sequences of operations that should be atomic
* **Detection**: Identifies critical sections that can be interleaved incorrectly
* **Analysis**: Checks for proper synchronization around related operations

**OpenMP Bugs**:

* **Taskgroup mismatch**: Detects ``__kmpc_taskgroup`` / ``__kmpc_end_taskgroup`` imbalance
* **Atomic region mismatch**: Detects ``__kmpc_atomic_start`` / ``__kmpc_atomic_end`` imbalance
* **Partial task synchronization**: Warns when ``__kmpc_omp_wait_deps`` may leave sibling tasks unsynchronized

**MPI Bugs**:

* **Orphaned request**: Non-blocking request without a matching ``Wait/Test/Request_free/Cancel``
* **Blocking deadlock**: Simple cycles involving blocking send/receive pairs
* **Collective mismatch**: Incompatible collective operations across ranks
* **Conditional collective**: Collectives that may execute only on a subset of ranks
* **Unsynchronized RMA / RMA race**: One-sided accesses without proven synchronization
* **Leaked window**: Window created but not freed

Analysis Process
----------------

1. **Parse LLVM IR**: Load module and identify concurrency primitives (threads, locks, synchronization)
2. **Build MHP Graph**: Determine which instructions may happen in parallel
3. **Lock Set Analysis**: Track which locks protect each memory access
4. **Happens-Before Analysis**: Build synchronization relationships (mutexes, barriers, etc.)
5. **Pattern Detection**: Identify shared-memory, OpenMP, and MPI-specific bug patterns
6. **Report Generation**: Generate reports through centralized ``BugReportMgr``

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Checker/Concurrency/ConcurrencyChecker.h"
   #include "Checker/Framework/BugReportMgr.h"
   
   using namespace concurrency;
   
   // Create checker
   ConcurrencyChecker checker(*module);
   
   // Configure checks
   checker.enableDataRaceCheck(true);
   checker.enableDeadlockCheck(true);
   checker.enableAtomicityCheck(true);
   checker.enableOpenMPCheck(true);
   checker.enableMPICheck(true);

   checker.runAnalyses();
   
   // Run checks (automatically reports to BugReportMgr)
   checker.runChecks();
   
   // Get statistics
   auto stats = checker.getStatistics();
   outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
   outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
   outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound << "\n";
   outs() << "OpenMP Bugs Found: " << stats.openMPBugsFound << "\n";
   outs() << "MPI Bugs Found: " << stats.mpiBugsFound << "\n";
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());

Statistics
----------

The checker provides detailed analysis statistics:

* ``totalInstructions`` – Total number of instructions analyzed
* ``mhpPairs`` – Number of instruction pairs that may happen in parallel
* ``locksAnalyzed`` – Number of lock operations analyzed
* ``dataRacesFound`` – Number of data races detected
* ``deadlocksFound`` – Number of deadlocks detected
* ``atomicityViolationsFound`` – Number of atomicity violations detected
* ``condVarBugsFound`` – Number of condition-variable misuse bugs detected
* ``lockMismatchesFound`` – Number of lock mismatch bugs detected
* ``openMPBugsFound`` – Number of OpenMP bugs detected
* ``mpiBugsFound`` – Number of MPI bugs detected

Limitations
-----------

* **Conservative Analysis**: May report false positives due to over-approximation
* **Protocol Coverage**: OpenMP and MPI support focuses on statically recognizable runtime patterns, not full semantic verification
* **Context-Insensitive**: Does not distinguish between different call contexts
* **No Dynamic Analysis**: Static analysis only, cannot detect runtime-specific issues

Performance
-----------

* Handles multiple threads efficiently using graph-based algorithms
* MHP analysis scales with the number of threads and instructions
* Lock set analysis is efficient for typical lock usage patterns
* Conservative analysis may have false positives but ensures soundness

Integration
-----------

The concurrency checker integrates with:

* **BugReportMgr**: Centralized bug reporting system
* **LLVM IR**: Standard LLVM intermediate representation
* **Dyck Alias Analysis**: Pointer analysis for memory access tracking

See Also
--------

- :doc:`index` – Checker Framework overview
- :doc:`../alias/index` – Alias analysis for pointer information
- :doc:`../analysis/index` – Analysis infrastructure
