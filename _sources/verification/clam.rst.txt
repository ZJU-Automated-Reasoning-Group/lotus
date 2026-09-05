CLAM – Abstract Interpretation Framework
==========================================

CLAM is a static analysis framework based on abstract interpretation over numerical abstract domains. It provides invariant generation and property checking for LLVM bitcode.

**Location**: ``third-party/verification/clam/src/``

**Headers**: ``third-party/verification/clam/include/Verification/clam/``

**Tools**: ``tools/verifier/clam/`` (command-line frontends)

Overview
--------

CLAM computes sound over-approximations of program behaviors by interpreting programs over different numerical abstract domains. It integrates with Lotus's alias analysis and memory modeling infrastructure.

**Key Features**:

* Multiple abstract domains: intervals, zones, octagons, polyhedra
* Memory safety checking: null pointer, buffer bounds, use-after-free
* Assertion checking and invariant generation
* Integration with Sea-DSA for precise heap abstraction
* Interprocedural and intraprocedural analysis modes

Components
----------

**Core Library** (``third-party/verification/clam/src/``):

* ``Clam.cc`` – Main analysis engine and pass registration
* ``CfgBuilder.cc`` – LLVM IR to CRAB IR translation
* ``CfgBuilderMemRegions.cc`` – Memory region abstraction
* ``SeaDsaHeapAbstraction.cc`` – Sea-DSA integration
* ``ClamQueryCache.cc`` – Query result caching

**Abstract Domains** (``third-party/verification/clam/src/crab/domains/``):

* Intervals, zones, octagons, polyhedra
* Disjunctive domains and term domains
* Domain-specific optimizations

**Property Checkers** (``third-party/verification/clam/src/Properties/``):

* ``NullCheck.cc`` – Null pointer dereference detection
* ``BndCheck.cc`` – Buffer bounds checking
* ``UafCheck.cc`` – Use-after-free detection
* ``MemoryCheckUtils.cc`` – Memory safety utilities

**Transforms** (``third-party/verification/clam/src/Transforms/``):

* ``DevirtFunctions.cc`` – Function devirtualization
* ``LowerSelect.cc`` – Select instruction lowering
* ``LowerUnsignedICmp.cc`` – Unsigned comparison lowering
* ``PromoteMalloc.cc`` – Malloc promotion
* ``NondetInit.cc`` – Non-deterministic initialization

**Command-Line Tools** (``tools/verifier/clam/``):

* ``clam`` – Main analyzer
* ``clam-pp`` – Preprocessing tool
* ``clam-diff`` – Differential analysis

Build Targets
-------------

* ``ClamAnalysis`` – Core library (``third-party/verification/clam/src/``)
* ``clam`` – Command-line analyzer
* ``clam-pp`` – Preprocessor
* ``clam-diff`` – Differential analyzer

Usage
------

**Basic Analysis**:

.. code-block:: bash

   ./build/bin/clam program.bc

**With Domain Selection**:

.. code-block:: bash

   ./build/bin/clam --crab-dom=zones program.bc
   ./build/bin/clam --crab-dom=pk program.bc  # Polyhedra

**Property Checking**:

.. code-block:: bash

   ./build/bin/clam --crab-check=assert program.bc
   ./build/bin/clam --crab-check=null program.bc
   ./build/bin/clam --crab-check=bounds program.bc

**Interprocedural Analysis**:

.. code-block:: bash

   ./build/bin/clam --crab-inter program.bc

**Preprocessing**:

.. code-block:: bash

   ./build/bin/clam-pp --crab-lower-unsigned-icmp \
                       --crab-lower-select \
                       input.bc -o prep.bc
   ./build/bin/clam prep.bc

Programmatic Usage
-------------------

.. code-block:: cpp

   #include "Verification/clam/Clam.hh"
   #include "Verification/clam/CfgBuilder.hh"
   
   // Create CFG builder
   CrabBuilderManager builder(module);
   
   // Create CLAM analysis
   GlobalClam ga(module, builder);
   
   // Run analysis
   AnalysisParams params;
   params.run_inter = true;
   ga.analyze(params);
   
   // Query invariants
   auto pre = ga.getPre(bb);
   auto post = ga.getPost(bb);

Abstract Domains
----------------

* **Intervals (int)**: Fast, value ranges ``[l, u]``
* **Zones**: Difference constraints ``x - y ≤ c``
* **Octagons (oct)**: Diagonal constraints ``±x ± y ≤ c``
* **Polyhedra (pk)**: Most precise, linear inequalities
* **Disjunctive domains**: Union of abstract values

See :doc:`../tools/verifier/clam/index` for detailed domain documentation and usage examples.

Integration Points
------------------

* **Sea-DSA**: Heap abstraction for memory modeling
* **Lotus Alias Analysis**: Pointer analysis integration
* **CRAB Library**: Abstract domain implementations
* **LLVM Pass Infrastructure**: Standard pass registration

See Also
--------

- :doc:`../tools/verifier/clam/index` – Detailed CLAM documentation and usage guide
- :doc:`../user_guide/tutorials` – CLAM usage examples
- :doc:`../alias/index` – Alias analysis for pointer information
- :doc:`../solvers/index` – Constraint solving backends
