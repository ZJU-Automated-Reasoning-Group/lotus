Checker Framework
==================

The Checker Framework provides a unified infrastructure for static bug detection across multiple vulnerability categories. It integrates various analysis techniques to detect security vulnerabilities, concurrency bugs, and numerical errors in LLVM bitcode.

**Location**: ``lib/Checker/``

**Headers**: ``include/Checker/``

**Tool Frontend**: ``lotus-check`` with engine runners in ``tools/checker/``

Overview
--------

The Checker Framework consists of several checker categories, all unified through a centralized bug reporting system:

* **AE Checkers** – Abstract-execution-based memory-safety bug detection
* **FiTx Checkers** – Daily development-friendly bug detection using typestate analysis (path-insensitive, return-code aware; see Suzuki et al., USENIX ATC 2024)
* **KINT Checkers** – Numerical bugs (overflow, division by zero, array bounds) using SMT solving and function summaries
* **IFDS Taint Checker** – Configurable source-to-sink information-flow analysis
* **Concurrency Checkers** – Thread safety and parallel-runtime issues (data races, deadlocks, atomicity violations, OpenMP bugs, MPI bugs) using MHP, lock set, OpenMP, and MPI analyses
* **Pulse Checker** – Memory safety and other bugs using biabductive analysis with path-sensitive interprocedural reasoning
* **Saber Checkers** – Source-sink bug detection over sparse value-flow graphs
* **SymEx Checker** – Symbolic-execution bug checks backed by the top-level ``SymbolicExecution`` engine

All checkers report bugs through the centralized ``BugReportMgr`` system, enabling unified output formats (JSON, SARIF) and consistent bug reporting across all analysis tools. The repository builds a single checker binary, ``lotus-check``; each invocation selects one engine with ``--engine=<name>``.

Choosing a Checker
------------------

The ``lotus-check --engine`` values name *analysis engines*, not mutually exclusive
vulnerability categories.  Several engines intentionally cover the same bug
class while making different precision, scalability, and reporting trade-offs.
Choose the engine according to the workflow and evidence needed; do not infer
that two engines with the same bug class have identical semantics or coverage.

The following table is a navigation aid for the currently exposed checks.  A
listed engine supports the corresponding class, but ``--checks=all`` only
enables that engine's own checks.

.. list-table:: Bug-class to engine guide
   :header-rows: 1
   :widths: 24 25 34 17

   * - Bug class
     - Start with
     - Other applicable engines
     - Typical use
   * - Use-after-free
     - ``pulse``
     - ``ae``, ``fitx``, ``symex``
     - Witness-oriented investigation
   * - Null-pointer dereference
     - ``pulse`` or ``ae``
     - ``fitx``, ``symex``
     - Memory-safety review
   * - Buffer or array bounds error
     - ``ae`` or ``kint``
     - ``pulse``, ``symex``
     - Buffer accesses or index arithmetic
   * - Integer overflow, division by zero, bad shift
     - ``kint``
     - ``symex``
     - Numerical-error analysis
   * - Memory leak
     - ``saber``
     - ``ae``, ``fitx``
     - Value-flow or typestate resource checking
   * - Double free
     - ``saber`` or ``fitx``
     - ``symex``
     - Allocation/free protocol checking
   * - Uninitialized read or use
     - ``pulse`` or ``fitx``
     - ``symex``
     - Initialization-state checking
   * - Tainted data reaches a sink
     - ``taint``
     - ``pulse``, ``symex``
     - Configurable source-to-sink analysis
   * - Data race, deadlock, or parallel-runtime error
     - ``concur``
     - —
     - Thread, OpenMP, or MPI analysis
   * - API protocol or project-specific policy
     - ``generic``
     - ``fitx`` for its built-in typestate checks
     - Declarative checks or fast development feedback

Quick selection
~~~~~~~~~~~~~~~

* Use ``fitx`` for fast, translation-unit-oriented feedback during routine
  development.
* Use ``ae`` for a broad abstract-execution memory-safety pass.
* Use ``pulse`` when a path-sensitive, witness-oriented diagnosis is most
  useful; its bounded analysis can miss bugs outside retained paths.
* Use ``symex`` when SMT-backed path feasibility and symbolic numeric reasoning
  are needed, accepting a potentially higher analysis cost.
* Use ``saber`` for sparse value-flow source/sink checks: memory leaks,
  double frees, and file-descriptor leaks.
* Use ``kint`` for numerical bugs.  Its taint analysis supports this purpose;
  it is not a replacement for the configurable ``taint`` source-to-sink tool.

Running multiple engines
~~~~~~~~~~~~~~~~~~~~~~~~

There is currently no aggregate command that runs multiple native engines or
deduplicates their output across separate ``lotus-check`` invocations.  Each
frontend may deduplicate reports produced in its own run, but findings from two
engines should be triaged as independent evidence. Record the engine value and
its options with exported JSON or SARIF reports so that overlapping findings
remain distinguishable.

Checker Framework
-----------------

The Checker Framework (``lib/Checker/Framework/``) provides the shared
infrastructure for defining and running bug checkers and for reporting their
findings. Declarative checkers are defined through specification files rather
than hardcoded C++ logic, enabling new checks without modifying the checker
engine.

**Components**:

* **CheckerSpec** — A declarative rule specification with metadata (id, title,
  severity, category), a rule kind (ForbiddenCall, SourceSink, ApiProtocol),
  and associated capabilities.
* **CheckerSpecLoader** — Loads ``CheckerSpec`` instances from YAML/JSON files
  or directories. Enables packaging reusable check rule sets.
* **CheckerRegistry** — Central registry that manages both declarative specs
  (loaded at runtime from spec files) and native checkers (compiled in).
  Supports id-based lookup, category filtering, and engine-kind selection.
* **CheckerDriver** — Orchestrates checker execution: selects and runs
  checkers from the registry over a ``CheckerContext``, collects diagnostics,
  and emits results to the ``BugReportMgr``.
* **CheckerDiagnostic** — Structured diagnostic with bug type, severity,
  source location, message, suggestion, confidence, and optional trace steps.
  Convertible to the ``BugReport`` format for unified reporting.
* **CheckerContext** — Per-module execution context providing the LLVM module
  and an optional alias-analysis wrapper.
* **CheckerValidator** — Validates checker specifications for consistency
  (e.g., missing required fields, unknown rule kinds).
* **BugReport** — Bug report data structures with source location information.
* **BugReportMgr** — Centralized bug report management (singleton pattern).
* **BugTypes** — Bug type definitions, classifications, and CWE mappings.
* **SARIF** — SARIF format output support.
* **ReportOptions** — Report configuration options (JSON, SARIF output).

**Rule Kinds**:

* **ForbiddenCall** — Flags calls to specified functions (e.g.,
  ``system``, ``gets``). Pattern: a simple function-name allow/block list.
* **SourceSink** — Tracks data flow from *sources* to *sinks* with optional
  sanitizers. Generalizes taint-style vulnerability detection.
* **ApiProtocol** — Checks acquire/use/release protocols for resources
  (locks, file handles, reference counts). Tracks state transitions and
  reports leaks, use-after-release, and double-acquire.
* **Native** — A hardcoded C++ checker that registers via the registry API.

**Usage**:

.. code-block:: bash

   # Run a declarative checker from a spec file
   ./build/bin/lotus-check --engine=generic input.bc --checks=forbidden.system

   # Load all specs from a directory
   ./build/bin/lotus-check --engine=generic input.bc --generic.spec-dir=./checker-specs/

   # List generic checker ids and native engine entries
   ./build/bin/lotus-check --list-checkers

.. code-block:: cpp

   #include "Checker/Framework/CheckerRegistry.h"
   #include "Checker/Framework/CheckerSpecLoader.h"

   lotus::checker::CheckerRegistry registry;
   lotus::checker::CheckerSpecLoader loader;

   // Load specs from a directory
   auto specs = loader.loadFromDirectory("./checker-specs/");
   for (const auto &spec : specs.get()) {
     registry.registerDeclarative(spec);
   }

   // Look up and run a checker
   auto *descriptor = registry.findById("forbidden.system");
   // ... execute via CheckerDriver

Components
----------

**Concurrency Checkers** (``lib/Checker/Concurrency/``):

* ``ConcurrencyChecker.cpp`` – Main concurrency checker coordinator
* ``DataRaceChecker.cpp`` – Data race detection using MHP (May Happen in Parallel) analysis
* ``DeadlockChecker.cpp`` – Deadlock detection using lock set analysis
* ``AtomicityChecker.cpp`` – Atomicity violation detection
* ``OpenMPChecker.cpp`` – Dedicated OpenMP bug checks built on OpenMP task analysis
* ``MPIChecker.cpp`` – Dedicated MPI bug checks built on MPI communication/RMA analysis

**AE Checkers** (``lib/Checker/AE/``):

* ``AbstractInterpretation.cpp`` – Fixpoint engine for abstract execution
* ``AEDetector.cpp`` – Bug-specific detector integration
* ``AbstractState.*`` / ``AbstractValue.*`` – State and value abstractions

**FiTx Bug Checkers** (``lib/Checker/FiTx/``):

* ``frontend/Framework.cpp`` – Main FiTx pass; typestate-based daily development-friendly checkers (Suzuki et al., USENIX ATC 2024)
* ``frontend/Analyzer.cpp`` – CFG-based typestate analysis with return-code aware state propagation
* ``Framework_IR/Analyzer.cpp`` – IR builder; collects return values for function summaries
* ``Detector/`` – Typestate definitions per bug pattern

**KINT Numerical Checkers** (``lib/Checker/KINT/``):

* ``MKintPass.cpp`` – Main KINT pass for integer overflow, division by zero, array bounds checking
* ``MKintBugreport.cpp`` – Bug report generation for KINT
* ``MKintSummary.cpp`` – Interprocedural SMT function-summary construction
* ``SummaryEncoding.cpp`` – SMT summary representation and instantiation
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

**Saber Checker** (``lib/Checker/Saber/``):

* ``LeakChecker.cpp`` – Leak detection over source-sink flows
* ``DoubleFreeChecker.cpp`` – Double-free detection
* ``FileChecker.cpp`` – File-descriptor leak checking
* ``SaberCheckerAPI.cpp`` – Reusable checker API surface

**Symbolic Execution Checker** (``lib/SymbolicExecution/``):

* ``AnalysisDriver.cpp`` – Whole-module symbolic execution driver
* ``AnalysisState*.cpp`` – Symbolic state, summaries, taint updates, and bug queries
* ``PathCondSolver.cpp`` – SMT-backed feasibility checking for path conditions
* ``SymbolicExecutionWrapper.cpp`` – LLVM pass wrapper used by ``lotus-check --engine=symex``

**Debug Info Analysis** (``lib/Analysis/DebugInfo/``):

* ``DebugInfoAnalysis.cpp`` – Debug information extraction from LLVM metadata

Build Targets
-------------

* ``FiTxChecker`` – FiTx typestate-based bug checker library
* ``PulseChecker`` – Pulse biabductive analysis checker library
* ``CanarySymbolicExecution`` – Symbolic-execution engine library used by ``symex``
* ``lotus-check`` – Unified checker frontend
* ``tools/checker/lotus-check-ae.cpp`` – AE engine runner
* ``tools/checker/lotus-check-fitx.cpp`` – FiTx engine runner
* ``tools/checker/lotus-check-kint.cpp`` – KINT engine runner
* ``tools/checker/lotus-check-concur.cpp`` – Concurrency engine runner
* ``tools/checker/lotus-check-pulse.cpp`` – Pulse engine runner
* ``tools/checker/lotus-check-saber.cpp`` – Saber engine runner
* ``tools/checker/lotus-check-symex.cpp`` – Symbolic-execution engine runner
* ``tools/checker/lotus-check-taint.cpp`` – Taint-analysis engine runner

Usage
-----

**KINT Tool**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all
   ./build/bin/lotus-check --engine=kint input.bc --checks=int-overflow,div-by-zero
   ./build/bin/lotus-check --engine=kint input.bc --report-json=report.json

**Concurrency Tool**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=concur input.bc --checks=data-race
   ./build/bin/lotus-check --engine=concur input.bc --checks=deadlock,atomicity
   ./build/bin/lotus-check --engine=concur input.bc --checks=openmp,mpi
   ./build/bin/lotus-check --engine=concur input.bc --report-json=report.json

**Pulse Tool**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=pulse input.bc
   ./build/bin/lotus-check --engine=pulse input.bc --verbose
   ./build/bin/lotus-check --engine=pulse input.bc --log-level=debug

Programmatic Usage
------------------

All checkers integrate with the centralized ``BugReportMgr``:

.. code-block:: cpp

   #include "Checker/Framework/BugReportMgr.h"
   
   // Access centralized reports emitted by checker frontends
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, BugReportMgr::ReportFilter{});

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

   ae
   concurrency
   fitx
   kint
   pulse
   report
   saber
   symex
   taint
   tooling
