=====================================
FPA — Function Pointer Analyses (FLTA/MLTA/MLTADF/KELP)
=====================================

Overview
========

The FPA module implements several **function pointer analysis** algorithms to
resolve indirect calls with different precision/performance trade-offs.

* **Location**: ``lib/Alias/Specialized/FPA``
* **Focus**: Indirect call resolution and call-graph construction
* **Algorithms**:
  - **FLTA** (1) – Flow-insensitive, type-based analysis
  - **MLTA** (2) – Multi-layer type analysis
  - **MLTADF** (3) – Multi-layer type analysis with data flow
  - **KELP** (4) – Context-sensitive analysis (USENIX Security'24)

Workflow
========

All FPA variants share a common high-level structure:

1. Scan the program to collect function pointer definitions and uses.
2. Build an abstract model of **types**, **call sites**, and **targets**.
3. Apply the selected algorithm (1–4) to approximate the mapping from call
   sites to possible function targets.
4. Optionally emit diagnostic or visualization output (e.g., call graphs).

Usage
=====

The analyses are exposed through the ``lotus-alias-fpa`` driver:

.. code-block:: bash

   ./build/bin/lotus-alias-fpa -analysis-type=1 example.bc  # FLTA
   ./build/bin/lotus-alias-fpa -analysis-type=2 -max-type-layer=10 example.bc  # MLTA

Key Options
-----------

* ``-analysis-type=<N>`` – Select analysis algorithm (1=FLTA, 2=MLTA, 3=MLTADF, 4=KELP)
* ``-max-type-layer=<N>`` – Set maximum type layer for MLTA analysis (default: 10)
* ``-debug`` – Enable debug output
* ``-output-file=<path>`` – Output file path for results (use "cout" for standard output)

Examples
--------

.. code-block:: bash

   # Using FLTA analysis
   ./build/bin/lotus-alias-fpa -analysis-type=1 input.bc

   # Using MLTA analysis with output to file
   ./build/bin/lotus-alias-fpa -analysis-type=2 -output-file=results.txt input.bc

   # Using KELP analysis with debug info
   ./build/bin/lotus-alias-fpa -analysis-type=4 -debug input.bc

FPA results can be consumed directly (for security analyses or refactoring)
or fed into other components that benefit from precise indirect call
resolution.
