Verifier Tools
==============

This page documents verification tools under ``tools/verifier/``. These tools
focus on program verification, abstract interpretation, and model checking.

CLAM – Numerical Abstract Interpretation
----------------------------------------

CLAM is a static analysis framework based on abstract interpretation over
numerical abstract domains. It is designed for inferring invariants and
checking safety properties over LLVM bitcode.

**Binaries**: ``clam``, ``clam-pp``, ``clam-diff``  
**Location**: ``tools/verifier/clam/``

**Quick Start**:

.. code-block:: bash

   # Analyze with interval domain
   ./build/bin/clam --crab-dom=int --crab-check=assert program.bc

   # Preprocess bitcode
   ./build/bin/clam-pp program.bc -o prep.bc

   # Compare analysis results
   ./build/bin/clam-diff baseline.json modified.json

For detailed documentation, see :doc:`clam/index`.

SymAbsAI – Symbolic Abstraction + Abstract Interpretation
------------------------------------------

SymAbsAI is a framework for static program analysis using symbolic abstraction
to provide a flexible interface for designing program analyses in a
compositional way.

**Binary**: ``lotus-verify-symabs-ai``
**Source**: ``tools/verifier/symabs-ai/lotus-verify-symabs-ai.cpp``

**Quick Start**:

.. code-block:: bash

   # Analyze all functions
   ./build/bin/lotus-verify-symabs-ai example.bc

   # Analyze specific function
   ./build/bin/lotus-verify-symabs-ai --function=foo example.bc

For detailed documentation, see :doc:`symabs-ai/index`.

Sifa – Symbolic Interpretation with Fluid Abstractions
------------------------------------------------------

Sifa is a symbolic-interpretation verifier with configurable abstract domains
and optional SymAbs-backed reasoning.

**Binary**: ``lotus-verify-sifa``
**Source**: ``tools/verifier/sifa/lotus-verify-sifa.cpp``

**Quick Start**:

.. code-block:: bash

   ./build/bin/lotus-verify-sifa program.bc
   ./build/bin/lotus-verify-sifa program.bc --function=foo --block=loop.header
   ./build/bin/lotus-verify-sifa program.bc --symabs --abstract-domain=Octagon

For detailed documentation, see :doc:`sifa/index`.

SeaHorn – Verification Framework
---------------------------------

SeaHorn is a large-scale SMT-based verification framework built on constrained
Horn clauses (CHC), symbolic execution, and abstraction-refinement.

**Binaries**: ``seahorn``, ``seapp``, ``seainspect``  
**Location**: ``tools/verifier/seahorn/``

**Quick Start**:

.. code-block:: bash

   # Bounded model checking
   ./build/bin/seahorn --bmc=10 program.c

   # CHC-based verification
   ./build/bin/seahorn --horn program.c

For detailed documentation, see :doc:`seahorn/index`.

Horn-ICE – CHC Verification with Learning
-----------------------------------------

Horn-ICE provides CHC (Constrained Horn Clause) verification with invariant
learning capabilities.

**Binaries**: ``chc_verifier``, ``hice-dt``  
**Location**: ``third-party/horn-ice/`` (moved from previous standalone location)

**Usage**:

.. code-block:: bash

   # CHC verification
   ./build/bin/chc_verifier input.smt2

   # CHC verification with learning
   ./build/bin/hice-dt input.smt2

**Build status**: built by default when ``LOTUS_ENABLE_HORN_ICE=ON``.

For detailed documentation, see :doc:`horn-ice/index`.

.. toctree::
   :maxdepth: 1

   clam/index
   sifa/index
   symabs-ai/index
   seahorn/index
   horn-ice/index
