Solver Tools
============

This page documents the command-line front-ends under ``tools/solver/``.
``lotus-datalog`` is built by default, while ``owl`` requires
``-DLOTUS_ENABLE_OWL=ON``. ``staub`` remains a source-present experimental
tool; SMT↔LLVM translation is provided by TUNA under ``lib/Solvers/SMT/TUNA``.

lotus-datalog – Datalog Solver Front-End
----------------------------------------

The native Datalog/lattice solver front-end accepts JSON Semantic IR, Lotus
Datalog, and Z3 fixedpoint input. It validates or executes programs and emits
canonical JSON relation rows and runtime statistics.

**Binary**: ``lotus-datalog``

**Source**: ``tools/solver/datalog/``

.. code-block:: bash

   ./build/bin/lotus-datalog schema > program.json
   ./build/bin/lotus-datalog validate program.json
   ./build/bin/lotus-datalog run program.json --workers 4 --pretty

OWL – SMT/Model Checking Front-End
----------------------------------

``owl`` is the supported solver front-end currently built from this directory.
It feeds SAT or SMT problems to the configured solver stack.

**Binary**: ``owl``  
**Location**: ``tools/solver/owl.cpp``

**Build status**: built only when ``-DLOTUS_ENABLE_OWL=ON``.

**Usage**:

.. code-block:: bash

   ./build/bin/owl file.smt2

**Example**:

.. code-block:: bash

   ./build/bin/owl examples/solver/example.smt2

See :doc:`../../solvers/smt` for details about the solver stack.

STAUB – Bounded-Theory Conversion Front-End
-------------------------------------------

``staub`` rewrites unbounded SMT constraints into bounded encodings before
translation or solving.

**Binary**: ``staub`` (source present, not built by default)

**Source**: ``tools/solver/staub.cpp``

This front-end is kept in the tree as an experimental source tool, not as a
default-built binary.

Basic usage:

.. code-block:: bash

   ./build/bin/staub -s query.smt2 -i aix -o bounded.smt2
   ./build/bin/staub -s query.smt2 -r 8,24 -o bounded.smt2

Important options:

- ``-s <file>`` – input SMT-LIB2 file
- ``-o <file>`` – output transformed formula
- ``-t <file>`` – write statistics
- ``-l`` – emit output compatible with SLOT
- ``-i <N|aix|aix2>`` – integer bounding mode
- ``-r <ebits,sbits|aix|aix4>`` – floating-point bounding mode
