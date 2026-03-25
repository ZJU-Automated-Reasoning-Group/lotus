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

**Binary**: ``symabs_ai``
**Location**: ``tools/verifier/symabs-ai/``

**Quick Start**:

.. code-block:: bash

   # Analyze all functions
   ./build/bin/symabs_ai example.bc

   # Analyze specific function
   ./build/bin/symabs_ai --function=foo example.bc

For detailed documentation, see :doc:`symabs-ai/index`.

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
**Location**: ``tools/verifier/horn-ice/``

**Usage**:

.. code-block:: bash

   # CHC verification
   ./build/bin/chc_verifier input.smt2

   # CHC verification with learning
   ./build/bin/hice-dt input.smt2

For detailed documentation, see :doc:`horn-ice/index`.

