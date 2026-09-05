TUNA
====

This section documents ``TUNA``, the compiler-optimization-based SMT
simplification toolkit in Lotus.

Overview
--------

**Location**: ``lib/Solvers/SMT/TUNA/``

**Origin**: https://dl.acm.org/doi/10.1145/3795879 ("Compiler
Optimization-Based SMT Simplifications: An In-Depth Study")

TUNA speeds up SMT solving by translating SMT-LIB 2 formulas to LLVM IR,
applying LLVM optimization passes, and translating the optimized IR back to
SMT-LIB 2. The simplified formula is typically faster to solve than the
original. A genetic algorithm (GA) searches for the best combination of LLVM
passes for a given benchmark dataset.

TUNA is a standalone subproject. It targets LLVM 16 (Lotus itself builds
against LLVM 14) and is not built by the default Lotus build. See
:ref:`tuna-build` below.

Components
----------

TUNA-Opt
~~~~~~~~

The core optimization framework under ``lib/Solvers/SMT/TUNA/TUNA-Opt/``.
It contains two modules:

- **SMT2LLVM** (``TUNA-Opt/SMT2LLVM/``): a bidirectional SMT-LIB 2 to LLVM IR
  conversion toolkit, extended from `SLOT <https://github.com/TUNA-SMT/SLOT>`_.
  Builds five command-line tools: ``slot``, ``fastslot``, ``smt2llvm``,
  ``llvm2smt``, and ``llvm2feat``.
- **GA** (``TUNA-Opt/GA/``): a genetic algorithm optimizer that searches for
  the optimal LLVM pass combination for a dataset of SMT files.

TUNA-Learn
~~~~~~~~~~

A planned machine-learning-based optimization framework under
``lib/Solvers/SMT/TUNA/TUNA-Learn/``. Currently a placeholder with no
implementation.

Build
-----

.. _tuna-build:

TUNA is not part of the default Lotus build. Build it from its own CMake
project:

.. code-block:: bash

   cd lib/Solvers/SMT/TUNA/TUNA-Opt/SMT2LLVM
   mkdir build && cd build
   cmake ..
   make -j 32

The build produces five binaries in ``build/``: ``slot``, ``fastslot``,
``smt2llvm``, ``llvm2smt``, and ``llvm2feat``.

**Dependencies**: LLVM 16.0.0, Z3 4.12.1, CMake + Ninja, Python 3. The
dependency paths are configured in ``SMT2LLVM/CMakeLists.txt``
(``LLVM_PATH`` and ``Z3_PATH``).

Tools
-----

slot
~~~~

Full pipeline: SMT-LIB 2 to LLVM IR, optimize, LLVM IR back to SMT-LIB 2.

.. code-block:: bash

   ./slot -m -s problem.smt2 -lu before.ll -lo after.ll -o simplified.smt2 -p ../passes-run.txt

Options (verified in ``src/tools/main.cpp``):

- ``-s <file>``: input SMT-LIB 2 file (required)
- ``-o <file>``: output SMT-LIB 2 file (default: stdout)
- ``-lu <file>``: write LLVM IR before optimization
- ``-lo <file>``: write LLVM IR after optimization
- ``-t <file>``: append timing statistics (CSV)
- ``-m``: convert constant right-shifts to multiplications
- ``-p <file>``: read the LLVM pass list from a file
- ``-h``: show help

fastslot
~~~~~~~~

Pass-file-driven pipeline for batch processing. Supports ``-s``, ``-o``,
``-lu``, ``-lo``, ``-t``, ``-m``, ``-p``, and ``-h``.

.. code-block:: bash

   ./fastslot -m -s input.smt2 -o result.smt2 -p ../passes-run.txt

smt2llvm
~~~~~~~~

Single-step SMT-LIB 2 to LLVM IR conversion.

.. code-block:: bash

   ./smt2llvm -s input.smt2 -lu output.ll

llvm2smt
~~~~~~~~

Single-step LLVM IR to SMT-LIB 2 conversion.

.. code-block:: bash

   ./llvm2smt -lo optimized.ll -o result.smt2

llvm2feat
~~~~~~~~~

Extracts program features (instruction counts, control flow, constant
statistics, etc.) from LLVM IR.

.. code-block:: bash

   ./llvm2feat -lo input.ll -f ./features/

Usage
-----

End-to-end SMT simplification:

.. code-block:: bash

   ./slot -m -s problem.smt2 -o simplified.smt2 -p ../passes-run.txt

Step-by-step with intermediate IR inspection:

.. code-block:: bash

   ./smt2llvm -s problem.smt2 -lu before.ll
   opt -passes="instcombine,gvn" -S before.ll -o after.ll
   ./llvm2smt -lo after.ll -o simplified.smt2

Pass Configuration Files
------------------------

The pass lists live in ``TUNA-Opt/SMT2LLVM/`` and follow the LLVM new Pass
Manager (``-passes=``) syntax:

+---------------------------+-------+------------------------------------------+
| File                      | Count | Description                              |
+===========================+=======+==========================================+
| ``passes-slot-old.txt``   | 9     | Core passes used by the original SLOT    |
+---------------------------+-------+------------------------------------------+
| ``passes-run.txt``        | 41    | All currently supported meaningful passes|
+---------------------------+-------+------------------------------------------+
| ``passes-filter.txt``     | 25    | Subset with clear optimization benefit   |
+---------------------------+-------+------------------------------------------+
| ``passes-useful.txt``     | 33    | Optimization-related passes              |
+---------------------------+-------+------------------------------------------+
| ``passes-16.txt``         | 80    | Base pass list available in LLVM 16      |
+---------------------------+-------+------------------------------------------+
| ``passes-all-llvm16.txt`` | 353   | Full LLVM 16 pass list                   |
+---------------------------+-------+------------------------------------------+

See ``TUNA-Opt/SMT2LLVM/FunctionPasses.md`` for pass descriptions.

GA Optimizer
------------

The GA under ``TUNA-Opt/GA/`` searches the boolean pass-enable space to
minimize total pipeline time:

::

   SMT -> smt2llvm -> LLVM IR -> opt (pass combination) -> LLVM IR -> llvm2smt -> SMT -> solver

Configure paths in ``GA/config.yaml`` (``slot_dir``, ``llvm_dir``,
``dataset_dir``, ``smt_solver_path``, ``output_csv_path``,
``smac_log_output_directory``), then run:

.. code-block:: bash

   cd lib/Solvers/SMT/TUNA/TUNA-Opt/GA
   python -m ga_optimizer

The GA runs 32 iterations with a population size of 32 and 40 parallel
processes. Pass combinations that beat the baseline solve time are written to
``output_csv_path`` in the format
``file_relative_path, cost_time, <pass1>, <pass2>, ..., status``.

Optional post-processing scripts:

- ``python -m delta_debugger``: prune redundant passes from the best
  combination, writing ``Refine_<original_filename>.csv``
- ``python -m phrase_time_counter``: compare total pipeline time across
  strategies (default solve, SLOT default passes, GA best, GA worst, ``-O3``)

Runtime logs go to ``~/logs/RQ1/`` (``rq1.info`` and ``rq1.error``).

Relationship to STAUB
---------------------

STAUB rewrites unbounded SMT constraints into bounded encodings. Its bounded
formulas can be translated to LLVM IR by TUNA's ``SMT2LLVM`` tools. See
:doc:`staub` for the STAUB flow.

See Also
--------

- :doc:`staub` - bounded-theory conversion that feeds TUNA's SMT2LLVM tools
- :doc:`smt` - the SMT solver backend
- :doc:`smtsampler` - SMT model sampling utilities
- :doc:`../tools/solver/index` - solver command-line front-ends