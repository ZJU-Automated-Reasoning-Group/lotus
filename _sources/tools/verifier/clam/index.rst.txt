CLAM – Numerical Abstract Interpretation
=========================================

CLAM is a static analysis framework based on abstract interpretation over
numerical abstract domains. It is designed for inferring invariants and
checking safety properties over LLVM bitcode.

**Location**: ``tools/verifier/clam/``

Overview
--------

CLAM computes sound over-approximations of program behaviors by interpreting
the program over different numerical domains.

**Headers**: ``third-party/verification/clam/include/Verification/clam``

**Implementation**: ``third-party/verification/clam/src`` (vendored core), ``lib/Verification/Backends`` (Lotus integration), and ``tools/verifier/clam`` (command-line frontends)

**Key features**:

- Multiple abstract domains: intervals, zones, octagons, polyhedra
- Memory safety checking (null, bounds, use-after-free)
- Assertion checking
- Integration with Sea-DSA for precise memory modeling

Internally, CLAM builds on the Crab numerical abstract interpretation library
and Lotus analysis infrastructure (CFG utilities, alias analysis, and memory
modeling) exposed under ``include/Analysis`` and related modules.

Abstract Domains
----------------

Choose a domain that balances precision and performance for your workload:

- **Interval (int)**:

  - Value ranges ``[l, u]``
  - Fastest and most scalable
  - Good for coarse invariants and quick checks

- **Zones**:

  - Difference constraints ``x - y ≤ c``
  - Captures simple relational properties between variables

- **Octagon (oct)**:

  - Diagonal constraints ``±x ± y ≤ c``
  - More precise than zones with moderate overhead

- **Polyhedra (pk)**:

  - Convex polyhedra ``∑ a_i x_i ≤ c``
  - Highest precision, highest cost
  - Suitable for small, critical kernels

Command-Line Usage
------------------

Invoke CLAM on LLVM bitcode:

.. code-block:: bash

   ./build/bin/clam [options] <input.bc>

Key Options
-----------

* ``--crab-dom=<domain>`` – Select abstract domain (int, zones, oct, pk, etc.)
* ``--crab-inter`` – Enable interprocedural analysis
* ``--crab-check=<type>`` – Enable property checking (assert, null, bounds, uaf)
* ``--crab-print-invariants=true`` – Print computed invariants
* ``-ojson=<file>`` – Output results in JSON format

Examples
--------

.. code-block:: bash

   # Interval domain analysis with assertion checking
   ./build/bin/clam --crab-dom=int --crab-check=assert program.bc

   # Interprocedural analysis with zones domain
   ./build/bin/clam --crab-inter --crab-dom=zones --crab-check=null program.bc

Preprocessing (clam-pp)
-----------------------

LLVM bitcode preprocessor for CLAM analysis.

Use ``clam-pp`` to normalize and simplify bitcode before analysis:

.. code-block:: bash

   ./build/bin/clam-pp [options] <input.bc> -o <output.bc>

Key Options
-----------

* ``--crab-lower-unsigned-icmp`` – Lower unsigned comparisons
* ``--crab-lower-select`` – Lower select instructions
* ``--crab-devirt`` – Perform devirtualization
* ``-o <file>`` – Output file

Differential Analysis (clam-diff)
---------------------------------

Compare JSON outputs from two CLAM analyses for differential verification.

.. code-block:: bash

   ./build/bin/clam-diff [options] <first.json> <second.json>

Key Options
-----------

* ``--dom=<domain>`` – Domain for semantic comparison
* ``--semdiff=<bool>`` – Enable semantic differencing (default: true)

Typical Workflow
----------------

1. **Preprocess**:

   .. code-block:: bash

      ./build/bin/clam-pp input.bc -o prep.bc

2. **Analyze**:

   .. code-block:: bash

      ./build/bin/clam \
         --crab-dom=zones \
         --crab-check=assert \
         prep.bc

3. **Export results** (optional):

   .. code-block:: bash

      ./build/bin/clam \
         --crab-dom=zones \
         --crab-check=assert \
         --crab-out=results.json \
         prep.bc
