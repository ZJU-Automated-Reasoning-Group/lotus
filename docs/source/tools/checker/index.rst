Checker Tools
=============

This page summarizes the bug-finding tools under ``tools/checker/``. For a
feature-oriented overview and examples, see :doc:`../../user_guide/bug_detection`.

lotus-kint – Integer and Array Bug Finder
-----------------------------------------

A static bug finder for integer-related and taint-style bugs (originally from
OSDI 12).

**Binary**: ``lotus-kint``  
**Location**: ``tools/checker/lotus_kint.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-kint [options] <input IR file>

**Bug Checker Options**:

* ``-check-all`` – Enable all checkers (overrides individual settings)
* ``-check-int-overflow`` – Enable integer overflow checker
* ``-check-div-by-zero`` – Enable division by zero checker
* ``-check-bad-shift`` – Enable bad shift checker
* ``-check-array-oob`` – Enable array index out of bounds checker
* ``-check-dead-branch`` – Enable dead branch checker

**Performance Options**:

* ``-function-timeout=<seconds>`` – Maximum time to spend analyzing a single
  function (0 = no limit, default: 10)

**Logging Options**:

* ``-log-level=<level>`` – Set logging level (debug, info, warning, error, none,
  default: info)
* ``-quiet`` – Suppress most log output
* ``-log-to-stderr`` – Redirect logs to stderr instead of stdout
* ``-log-to-file=<filename>`` – Redirect logs to the specified file

**Detects**:

The tool detects various integer-related bugs:

1. **Integer overflow**: Detects potential integer overflow in arithmetic operations
2. **Division by zero**: Detects potential division by zero errors
3. **Bad shift**: Detects invalid shift amounts
4. **Array out of bounds**: Detects potential array index out of bounds access
5. **Dead branch**: Detects impossible branches in conditional statements

**Examples**:

.. code-block:: bash

   # Enable all checkers
   ./build/bin/lotus-kint -check-all input.ll

   # Enable specific checkers
   ./build/bin/lotus-kint -check-int-overflow -check-div-by-zero input.ll

   # Set function timeout and log level
   ./build/bin/lotus-kint -function-timeout=30 -log-level=debug input.ll

   # Quiet mode with output to file
   ./build/bin/lotus-kint -quiet -log-to-file=analysis.log input.ll

lotus-taint – Taint Analysis
----------------------------

IFDS-based interprocedural taint analysis tool.

**Binary**: ``lotus-taint``  
**Location**: ``tools/checker/lotus_taint.cpp``

**Detects**:

- Flows from untrusted sources (e.g., input) to sensitive sinks (e.g., system calls)
- Optional reaching-definitions analysis mode

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-taint [options] input.bc

Key options:

- ``-analysis=0`` – Taint analysis (default)
- ``-analysis=1`` – Reaching-definitions analysis
- ``-sources=<f1,f2,...>`` – Custom source functions
- ``-sinks=<f1,f2,...>`` – Custom sink functions
- ``-max-results=<N>`` – Limit number of detailed flows
- ``-verbose`` – Detailed path information

For end-to-end examples (command injection, SQL injection, etc.), see
:doc:`../../user_guide/bug_detection` and :doc:`../../dataflow/ifds_ide`.

lotus-concur – Concurrency Bug Checker
---------------------------------------

Static analysis for shared-memory concurrency bugs plus dedicated OpenMP and
MPI checks.

**Binary**: ``lotus-concur``  
**Location**: ``tools/checker/lotus_concur.cpp``

**Detects**:

- Data races on shared variables
- Locking discipline violations
- Potential deadlocks (lock ordering issues)
- OpenMP taskgroup/atomic-region mismatches and partial task synchronization
- MPI request lifecycle bugs, collective mismatches, simple MPI deadlocks, and RMA issues

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-concur [options] input.bc

Key options:

- ``--checks=race,deadlock,atomicity,condvar,lock-mismatch,openmp,mpi`` – enable only the listed checks
- ``--check-openmp`` – run dedicated OpenMP checks
- ``--check-mpi`` – run dedicated MPI checks
- ``--analysis-only`` – dump analysis facts without bug emission

Typical workflow:

1. Compile multi-threaded C/C++ program to LLVM bitcode
2. Run ``lotus-concur`` on the bitcode
3. Inspect reported shared variables and threads involved in races

Detailed concurrency examples and recommended patterns are in
:doc:`../../user_guide/bug_detection`.
