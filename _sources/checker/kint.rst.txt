KINT Numerical Bug Checker
===========================

Static analysis tool for detecting numerical bugs: integer overflow, division
by zero, array bounds violations, and related issues.

**Library Location**: ``lib/Checker/KINT/``

**Headers**: ``include/Checker/KINT/``

**Tool Location**: ``tools/checker/lotus-check-kint.cpp``

**Frontend**: ``lotus-check --engine=kint``

Overview
--------

KINT (Kint Is Not Taint) uses SMT solving and summary encoding to detect
numerical bugs in LLVM bitcode. It combines:

* **SMT Solving**: Z3-based path-sensitive verification for precise bug detection
* **Summary Encoding**: Inter-procedural constraint encoding via function summaries
* **Taint Analysis**: Tracking of untrusted data sources

.. note::
   Range analysis was removed (commit 88adc045) and replaced with a pure
   SMT-based approach using per-object memory arrays and inter-procedural
   function summaries.

All detected bugs are reported through the centralized ``BugReportMgr`` system, enabling unified JSON and SARIF output.

Components
----------

**MKintPass** (``MKintPass.cpp``, ``MKintPass.h``):

* Main LLVM pass that orchestrates all KINT analyses
* Integrates SMT solving, summary encoding, taint analysis, and bug detection
* Reports bugs through ``BugReportMgr``

**SummaryEncoding** (``SummaryEncoding.cpp``, ``SummaryEncoding.h``):

* Defines ``FunctionSummary``, ``SummaryObjectBinding``, ``SummaryCacheEntry``
* Built by ``MKintSummary.cpp`` (1032 lines) — the summary building pipeline
* Supports ``--kint.summary-mode off|on|required`` (default: on)
* Building workflow: ``canSummarizeFunction()`` → ``collectSummaryObjects()`` → ``buildSummary()`` (symbolic execution of callee) → ``applySummary()`` (instantiation at call site)
* Per-summary timeout via ``--kint.summary-timeout-seconds <seconds>`` (default: 5)
* Path budget via ``--kint.summary-max-paths <N>`` (default: 64)

**KINTTaintAnalysis** (``KINTTaintAnalysis.cpp``, ``KINTTaintAnalysis.h``):

* Tracks taint sources (untrusted inputs)
* Propagates taint through the program
* Identifies tainted values used in security-critical operations

**BugDetection** (``BugDetection.cpp``, ``BugDetection.h``):

* Detects specific bug patterns:
  * Integer overflow
  * Division by zero
  * Bad shift operations
  * Array out-of-bounds access
  * Dead branches (unreachable code)

**MKintSummary** (``MKintSummary.cpp``, ``MKintSummary.h``):

* Builds and applies inter-procedural function summaries as Z3 contracts
* Captures integer/pointer parameters, return values, memory side effects, and path constraints
* Handles summary caching with ``building`` flag for recursive call cycle detection

**Options** (``Options.cpp``, ``Options.h``):

* Command-line option parsing and configuration
* Checker enable/disable flags
* Performance tuning options

**Log** (``Log.cpp``, ``Log.h``):

* Logging infrastructure with configurable levels
* Uses the shared checker ``--log-level`` policy and writes diagnostics to stderr

**Utils** (``Utils.cpp``, ``Utils.h``):

* Utility functions for analysis

Usage
-----

**Enable All Checkers**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc

**Enable Specific Checkers**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=int-overflow
   ./build/bin/lotus-check --engine=kint input.bc --checks=div-by-zero
   ./build/bin/lotus-check --engine=kint input.bc --checks=bad-shift
   ./build/bin/lotus-check --engine=kint input.bc --checks=array-oob
   ./build/bin/lotus-check --engine=kint input.bc --checks=dead-branch

**Enable Multiple Checkers**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=int-overflow,div-by-zero

**Generate JSON Report**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all --report-json=report.json

**Generate SARIF Report**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all --report-sarif=report.sarif

**Verbose Logging**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all --log-level=debug

**Function Timeout**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all --kint.function-timeout-seconds=60

**Analyze All Functions**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=int-overflow --kint.analyze-all-functions=true

**Summary Encoding Options**:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all --kint.summary-mode=on
   ./build/bin/lotus-check --engine=kint input.bc --checks=all --kint.summary-timeout-seconds=10
   ./build/bin/lotus-check --engine=kint input.bc --checks=all --kint.summary-max-paths=128

Command-Line Options
--------------------

**Checker Options**:

* ``--checks=<id[,id...]>`` – Select ``int-overflow``, ``div-by-zero``,
  ``bad-shift``, ``array-oob``, or ``dead-branch``. Omitting this option, or
  passing ``--checks=all``, enables all KINT checks.
* ``--kint.analyze-all-functions=<true|false>`` – Run SMT checks for all functions instead of only taint/main entry points (default: false)

**Performance Options**:

* ``--kint.function-timeout-seconds=<seconds>`` – Timeout per function for SMT solving (0 = no limit, default: varies)
* ``--kint.summary-mode=<off|on|required>`` – Control inter-procedural summary building (default: on)
* ``--kint.summary-timeout-seconds=<seconds>`` – Per-summary timeout (default: 5)
* ``--kint.summary-max-paths=<N>`` – Path budget for summary construction (default: 64)

**Logging Options**:

* ``--log-level=<debug|info|warning|error|none>`` – Set logging level (default: warning)

**Robust reachability options**:

* ``--kint.robust-checks=<id[,id...]>`` – Restrict robust reachability to a subset
  of the canonical IDs selected by ``--checks``.

**Report Options**:

* ``--report-json=<file>`` – Output JSON report to file
* ``--report-sarif=<file>`` – Output SARIF report to file
* ``--report-min-score=<n>`` – Minimum confidence score for reporting (0-100)

Bug Types
---------

**Integer Overflow** (``Integer Overflow``):

* **CWE**: CWE-190
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Arithmetic operations that may overflow integer bounds

**Divide by Zero** (``Divide by Zero``):

* **CWE**: CWE-369
* **Importance**: MEDIUM
* **Classification**: ERROR
* **Description**: Division operations where the divisor may be zero

**Bad Shift** (``Bad Shift``):

* **Importance**: MEDIUM
* **Classification**: ERROR
* **Description**: Shift operations with invalid shift amounts

**Array Out of Bounds** (``Array Out of Bounds``):

* **CWE**: CWE-119, CWE-125
* **Importance**: HIGH
* **Classification**: SECURITY
* **Description**: Array accesses that may be outside array bounds

**Dead Branch** (``Dead Branch``):

* **Importance**: LOW
* **Classification**: ERROR
* **Description**: Unreachable code branches

Analysis Process
----------------

1. **Taint Source Identification**: Mark taint sources (untrusted inputs)
2. **Summary Building**: Build inter-procedural function summaries for all functions
3. **SMT Solving**: Use Z3 with per-object memory model and summary application at call sites
4. **Bug Detection**: Identify specific bug patterns based on SMT results
5. **Report Generation**: Generate reports through centralized ``BugReportMgr``

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Checker/KINT/MKintPass.h"
   #include "Checker/Framework/BugReportMgr.h"
   
   using namespace kint;
   
   // Create and run the pass
   llvm::ModuleAnalysisManager MAM;
   llvm::ModulePassManager MPM;
   llvm::PassBuilder PB;
   
   PB.registerModuleAnalyses(MAM);
   MPM.addPass(kint::MKintPass());
   
   // Run analysis (automatically reports to BugReportMgr)
   MPM.run(*M, MAM);
   
   // Access centralized reports
   BugReportMgr& mgr = BugReportMgr::get_instance();
   mgr.print_summary(outs());
   mgr.generate_json_report(jsonFile, BugReportMgr::ReportFilter{});

SMT-based numerical reasoning
-----------------------------

For path-sensitive verification, KINT uses Z3:

* **Path Constraints**: Builds constraints along execution paths
* **Symbolic Execution**: Creates symbolic expressions for variables
* **Satisfiability Checking**: Verifies if bug conditions are satisfiable
* **Timeout Handling**: Limits analysis time per function

Memory modeling was improved (commit b4cef8a1) with per-object SMT arrays: each allocation site gets its own byte array (``obj_base``, ``obj_size``, ``obj_mem``), with ``ObjectStateFrame`` snapshots for path branching, alias tracking (``m_obj_alias``, ``m_int_alias``), and selective havoc for unknown calls.

Taint Analysis
--------------

KINT tracks taint sources and propagation to focus its numerical-bug analysis:

* **Taint Sources**: Functions that read untrusted input (e.g., ``read()``, ``recv()``)
* **Taint Propagation**: Tracks how tainted values flow through the program
* **Taint Sinks**: Security-critical operations that use tainted data

This auxiliary tracking is not a general-purpose taint-reporting frontend.  To
ask whether configured data sources reach configured sinks, use
:doc:`taint` instead.

Limitations
-----------

* **SMT Solver Timeout**: Complex functions may timeout, leading to incomplete analysis
* **Loop Handling**: Complex loops may require manual widening hints
* **Floating Point**: Limited support for floating-point operations
* **Context Sensitivity**: Intraprocedural analysis may miss interprocedural bugs

Performance
-----------

* SMT solving can be slow for complex functions (use timeouts)
* Function timeout helps prevent analysis from getting stuck
* Statistics can help identify performance bottlenecks

Integration
-----------

KINT integrates with:

* **Z3 SMT Solver**: Path-sensitive verification
* **LLVM Pass Infrastructure**: Standard pass registration
* **BugReportMgr**: Centralized bug reporting system
* **Auxiliary Taint Analysis**: Focuses numerical analysis on untrusted data

See Also
--------

- :doc:`index` – Checker Framework overview
- :doc:`../solvers/index` – SMT solver integration
- :doc:`../analysis/index` – Analysis infrastructure
- :doc:`taint` – configurable IFDS source-to-sink analysis
