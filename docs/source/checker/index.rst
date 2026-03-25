Checker Framework
==================

The Checker Framework provides a unified infrastructure for static bug detection across multiple vulnerability categories. It integrates various analysis techniques to detect security vulnerabilities, concurrency bugs, and numerical errors in LLVM bitcode.

**Location**: ``lib/Checker/``

**Headers**: ``include/Checker/``

**Tools**: ``tools/checker/`` (command-line frontends)

Overview
--------

The Checker Framework consists of four main checker categories, all unified through a centralized bug reporting system:

* **FiTx Checkers** – Daily development-friendly bug detection using typestate analysis (path-insensitive, return-code aware; see Suzuki et al., USENIX ATC 2024)
* **KINT Checkers** – Numerical bugs (overflow, division by zero, array bounds) using range analysis and SMT solving
* **Concurrency Checkers** – Thread safety and parallel-runtime issues (data races, deadlocks, atomicity violations, OpenMP bugs, MPI bugs) using MHP, lock set, OpenMP, and MPI analyses
* **Pulse Checker** – Memory safety and other bugs using biabductive analysis with path-sensitive interprocedural reasoning

All checkers report bugs through the centralized ``BugReportMgr`` system, enabling unified output formats (JSON, SARIF) and consistent bug reporting across all analysis tools.

Components
----------

**Concurrency Checkers** (``lib/Checker/Concurrency/``):

* ``ConcurrencyChecker.cpp`` – Main concurrency checker coordinator
* ``DataRaceChecker.cpp`` – Data race detection using MHP (May Happen in Parallel) analysis
* ``DeadlockChecker.cpp`` – Deadlock detection using lock set analysis
* ``AtomicityChecker.cpp`` – Atomicity violation detection
* ``OpenMPChecker.cpp`` – Dedicated OpenMP bug checks built on OpenMP task analysis
* ``MPIChecker.cpp`` – Dedicated MPI bug checks built on MPI communication/RMA analysis

**FiTx Bug Checkers** (``lib/Checker/FiTx/``):

* ``frontend/Framework.cpp`` – Main FiTx pass; typestate-based daily development-friendly checkers (Suzuki et al., USENIX ATC 2024)
* ``frontend/Analyzer.cpp`` – CFG-based typestate analysis with return-code aware state propagation
* ``framework_ir/Analyzer.cpp`` – IR builder; collects return values for function summaries
* ``detector/df_detector/``, ``uaf_detector/``, ``leak_detector/``, etc. – Typestate definitions per bug pattern

**KINT Numerical Checkers** (``lib/Checker/KINT/``):

* ``MKintPass.cpp`` – Main KINT pass for integer overflow, division by zero, array bounds checking
* ``MKintPass_bugreport.cpp`` – Bug report generation for KINT
* ``RangeAnalysis.cpp`` – Range analysis for variables using abstract interpretation
* ``KINTTaintAnalysis.cpp`` – Taint analysis integration for tracking untrusted data
* ``BugDetection.cpp`` – Bug detection and reporting logic
* ``Options.cpp`` – Command-line option parsing
* ``Log.cpp`` – Logging utilities
* ``Utils.cpp`` – Utility functions

**Pulse Checker** (``lib/Checker/Pulse/``):

* ``PulseChecker.cpp`` – Main bug finder using biabductive analysis
* ``PulseDomain.cpp`` – Execution domain abstraction
* ``PulseAbductiveDomain.h`` – Core abstract domain with biabduction
* ``PulseOperations.cpp`` – Core memory operations (readDeref, writeDeref, etc.)
* ``PulseDisjunctiveDomain.cpp`` – Disjunctive analysis for path-sensitive reasoning
* ``PulseLoopAbstraction.cpp`` – Loop abstraction with widening
* ``PulseSummary.cpp`` – Function summary representation and application
* ``PulseTaint.cpp`` – Taint analysis for security vulnerabilities
* ``PulseModels.cpp`` – Library function models
* ``PulseDiagnostic.cpp`` – Rich diagnostic reporting with traces

**Report System** (``lib/Checker/Report/``):

* ``BugReport.cpp`` – Bug report data structures with source location information
* ``BugReportMgr.cpp`` – Centralized bug report management (singleton pattern)
* ``BugTypes.cpp`` – Bug type definitions, classifications, and CWE mappings
* ``SARIF.cpp`` – SARIF format output support
* ``ReportOptions.cpp`` – Report configuration options (JSON, SARIF output)

**Debug Info Analysis** (``lib/Analysis/DebugInfo/``):

* ``DebugInfoAnalysis.cpp`` – Debug information extraction from LLVM metadata

Build Targets
-------------

* ``FiTxChecker`` – FiTx typestate-based bug checker library
* ``PulseChecker`` – Pulse biabductive analysis checker library
* ``lotus-fitx`` – FiTx daily development-friendly bug checker (``tools/checker/lotus_fitx.cpp``)
* ``lotus-kint`` – KINT numerical bug detection tool (``tools/checker/lotus_kint.cpp``)
* ``lotus-concur`` – Concurrency checker tool (``tools/checker/lotus_concur.cpp``)
* ``lotus-pulse`` – Pulse biabductive analysis tool (``tools/checker/lotus_pulse.cpp``)
* ``lotus-taint`` – Taint analysis tool (``tools/checker/lotus_taint.cpp``)

Usage
-----

**KINT Tool**:

.. code-block:: bash

   ./build/bin/lotus-kint --check-all=true input.bc
   ./build/bin/lotus-kint --check-int-overflow=true --check-div-by-zero=true input.bc
   ./build/bin/lotus-kint --report-json=report.json input.bc

**Concurrency Tool**:

.. code-block:: bash

   ./build/bin/lotus-concur --check-data-races input.bc
   ./build/bin/lotus-concur --check-deadlocks --check-atomicity input.bc
   ./build/bin/lotus-concur --checks=openmp,mpi input.bc
   ./build/bin/lotus-concur --report-json=report.json input.bc

**Pulse Tool**:

.. code-block:: bash

   ./build/bin/lotus-pulse input.bc
   ./build/bin/lotus-pulse -v input.bc
   ./build/bin/lotus-pulse --log-level=debug input.bc

Programmatic Usage
------------------

All checkers integrate with the centralized ``BugReportMgr``:

.. code-block:: cpp

   #include "Checker/Report/BugReportMgr.h"
   
   // Access centralized reports emitted by checker frontends
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, 0);

Bug Types
---------

The framework detects the following bug categories:

* **Memory Safety**: Null pointer dereference, use-after-free, uninitialized reads, invalid accesses, memory leaks
* **Numerical Errors**: Integer overflow, division by zero, bad shift, array out-of-bounds, dead branches
* **Concurrency**: Data races, deadlocks, atomicity violations, lock mismatches, condition-variable misuse, OpenMP runtime misuse, MPI protocol/RMA bugs
* **Security**: Taint errors (untrusted data flows), use-after-free, null dereferences
* **Performance**: Unnecessary copies, const-refable parameters

All bug types are classified by importance (LOW, MEDIUM, HIGH) and category (SECURITY, ERROR, WARNING, PERFORMANCE) with CWE mappings.

Integration Points
------------------

* **UnderApproxAA**: Used by PulseChecker for must-alias canonicalization
* **Z3 SMT Solver**: Used by KINT for path-sensitive verification
* **Biabductive Analysis**: Used by PulseChecker for precise bug detection
* **LLVM Pass Infrastructure**: Standard pass registration for integration

See Also
--------

.. toctree::
   :maxdepth: 1

   concurrency
   fitx
   kint
   pulse
