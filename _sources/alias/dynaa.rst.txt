========================
DynAA — Dynamic Alias AA
========================

Overview
========

DynAA provides **dynamic validation** for static alias analyses. It
instruments programs to record runtime points-to information, then compares
these logs against the results of a chosen static AA.

* **Static side**: e.g., SparrowAA, AserPTA, LotusAA.
* **Dynamic side**: runtime traces collected by DynAA
* **Location**: ``tools/alias/dynaa``

Workflow
========

The typical workflow is:

1. **Compile** the test program to LLVM IR:

   .. code-block:: bash

      clang -emit-llvm -c example.cpp -o example.bc

2. **Instrument** the code for dynamic analysis:

   .. code-block:: bash

      ./build/bin/dynaa-instrument example.bc -o example.inst.bc

3. **Compile** the instrumented IR and link with the runtime library:

   .. code-block:: bash

      clang example.inst.bc libRuntime.a -o example.inst

4. **Run** the instrumented program to collect runtime pointer information:

   .. code-block:: bash

      LOG_DIR=logs/ ./example.inst

5. **Check** the collected information against a static alias analysis:

   .. code-block:: bash

      ./build/bin/dynaa-check example.bc logs/pts.log basic-aa

To dump the binary log files to a readable format:

.. code-block:: bash

   ./build/bin/dynaa-log-dump <log-dir>/pts.log

Tools
=====

* ``dynaa-instrument``: Instrument code for runtime tracking
* ``dynaa-check``: Validate static analysis against runtime
* ``dynaa-log-dump``: Convert binary logs to readable format

Features and Use Cases
======================

* Detects mismatches between static AA predictions and observed behavior.
* Helps evaluate **precision** and **soundness** of new alias analyses.
* Useful for regression testing and benchmarking AA implementations.





