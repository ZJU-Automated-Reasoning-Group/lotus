SeaHorn – Verification Framework
=================================

SeaHorn is a large-scale SMT-based verification framework built on constrained
Horn clauses (CHC), symbolic execution, and abstraction-refinement.

**Binaries**: ``seahorn``, ``seapp``, ``seainspect``  
**Location**: ``tools/verifier/seahorn/``

For detailed framework documentation, see :doc:`../../../verification/seahorn`.

Overview
--------

SeaHorn performs bounded and unbounded model checking on LLVM bitcode using SMT
solvers to prove program correctness or produce counterexamples. It integrates
with Lotus analyses and solver backends to support complex verification
pipelines.

Command-Line Tools
------------------

SeaHorn Verification (seahorn)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Main verification tool for C programs.

**Basic usage**:

.. code-block:: bash

   ./build/bin/seahorn [options] <input.c>

**Common modes**:

- ``--bmc=<N>`` – Bounded model checking up to ``N`` steps.
- ``--horn`` – CHC-based (unbounded) verification.
- ``--abstractor=clam`` – Use CLAM-based abstract interpretation as an
  abstractor.

**Frequently used options**:

- ``--cex=<file>`` – Dump a counterexample harness to ``<file>``.
- ``--track=mem`` – Track memory (heap/stack) explicitly.
- ``--horn-solver=spacer|ice`` – Select CHC solving engine.

**Example**:

.. code-block:: bash

   # Bounded model checking with 10 steps
   ./build/bin/seahorn --bmc=10 program.c

   # CHC-based verification
   ./build/bin/seahorn --horn program.c

   # Generate counterexample
   ./build/bin/seahorn --cex=harness.ll program.c
   clang -m64 -g program.c harness.ll -o counterexample

SeaHorn Preprocessor (seapp)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

LLVM bitcode preprocessing tool for SeaHorn.

**Usage**:

.. code-block:: bash

   ./build/bin/seapp [options] input.bc -o output.bc

**Common options**:

- ``--horn-make-undef-warning-error`` – Treat undefined value warnings as errors
- ``--strip-extern`` – Strip external function declarations
- ``--simplify-cfg`` – Simplify control flow graph

SeaHorn Inspector (seainspect)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Tool for inspecting and analyzing SeaHorn verification results.

**Usage**:

.. code-block:: bash

   ./build/bin/seainspect [options] input.bc

Counterexample Analysis
-----------------------

Generate executable counterexamples:

.. code-block:: bash

   ./build/bin/seahorn --cex=harness.ll program.c
   clang -m64 -g program.c harness.ll -o counterexample
   ./counterexample

Integration with Other Tools
----------------------------

SeaHorn can be integrated with:

- **CLAM** – Use ``--abstractor=clam`` to use CLAM abstract interpretation
- **Horn-ICE** – Use ``--horn-solver=ice`` for CHC solving with learning
- **Z3/Spacer** – Default CHC solver (``--horn-solver=spacer``)

For more details on the SeaHorn framework architecture and components, see
:doc:`../../../verification/seahorn`.

