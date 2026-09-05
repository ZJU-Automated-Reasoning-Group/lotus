Horn-ICE – CHC Verification with Learning
==========================================

Horn-ICE provides CHC (Constrained Horn Clause) verification with invariant
learning capabilities using decision trees.

**Binaries**: ``chc_verifier``, ``hice-dt``  
**Location**: ``third-party/horn-ice/``

Overview
--------

Horn-ICE is an online learning-based approach for synthesizing invariants and
contracts for Constrained Horn Clauses. It uses decision trees to learn
invariants from positive and negative examples.

**Key features**:

- CHC verification with invariant learning
- Decision tree-based invariant synthesis
- Integration with SeaHorn for CHC generation
- Support for SMT-LIB2 datalog format

The implementation is adapted from the original Horn-ICE project
(https://github.com/horn-ice/hice-dt) and integrated into Lotus.

Command-Line Tools
------------------

CHC Verifier (chc_verifier)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A verifier for Constrained Horn Clauses built on top of Microsoft Z3. It
verifies constrained Horn clauses in the SMT-LIB2 datalog format (e.g., as
produced by SeaHorn).

**Usage**:

.. code-block:: bash

   ./build/bin/chc_verifier input.smt2

**Input format**: SMT-LIB2 datalog format (CHC format)

**Example**:

.. code-block:: bash

   # Generate CHCs from a program using SeaHorn
   ./build/bin/seahorn --horn program.c -o program.smt2

   # Verify using Horn-ICE
   ./build/bin/chc_verifier program.smt2

Horn-ICE Decision Tree (hice-dt)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

CHC verification with decision tree-based invariant learning.

**Usage**:

.. code-block:: bash

   ./build/bin/hice-dt input.smt2

**Features**:

- Online learning from positive and negative examples
- Decision tree-based invariant synthesis
- Automatic invariant refinement

**Example**:

.. code-block:: bash

   # CHC verification with learning
   ./build/bin/hice-dt program.smt2

Workflow
--------

Typical workflow for using Horn-ICE:

1. **Generate CHCs** from your program using SeaHorn:

   .. code-block:: bash

      ./build/bin/seahorn --horn program.c -o program.smt2

2. **Verify using Horn-ICE**:

   .. code-block:: bash

      # Using basic CHC verifier
      ./build/bin/chc_verifier program.smt2

      # Or using decision tree learning
      ./build/bin/hice-dt program.smt2

Benchmarks
----------

The Horn-ICE distribution includes benchmarks from the HOLA benchmark suite
(45 sequential programs without recursion from Dillig et al.) located in
``third-party/horn-ice/benchmarks/``.

**Related Work**:

- OOPSLA 18: Horn-ICE learning for synthesizing invariants and contracts
  https://dl.acm.org/doi/10.1145/3276501
- Online demo: https://horn-ice.mpi-sws.org/

**Baselines**:

- Ultimate Automizer
- Z3 PDR (the Spacer engine)

For more information, see the Horn-ICE README at ``third-party/horn-ice/README.md``.
