CFL Tools
=========

This page documents the CFL-related tools under ``tools/cfl/``. For the
underlying theory and components, see :doc:`../../cfl/cfl_components`.

Overview
--------

Context-Free Language (CFL) reachability extends graph reachability with
context-free grammars for precise interprocedural analysis. CFL reachability
enables analysis of complex program properties using grammar-based constraints.

**Location**: ``tools/cfl/``

**Tools**: ``lotus-cfl-mcfl`` (MCFL reachability), CSR (indexed CFL
reachability)

MCFL: Multiple Context-Free Language Reachability
-------------------------------------------------

Runs the POPL 2025 MCFL hierarchy for underapproximating interleaved-Dyck
reachability on artifact-compatible DOT graphs.

**Binary**: ``lotus-cfl-mcfl``

**Location**: ``tools/cfl/mcfl/lotus-cfl-mcfl.cpp``

.. code-block:: bash

   cmake -S . -B build -DLOTUS_ENABLE_CFL=ON
   cmake --build build --target lotus-cfl-mcfl
   ./build/bin/lotus-cfl-mcfl --dimension 2 input.dot

Useful options include ``--simple`` for the weaker ``G_d^circ`` grammar,
``--no-condense`` to disable cycle elimination, ``--stats`` for saturation
counters, ``--artifact-compatible`` for exact condensed cross-product
expansion, ``--print-pairs`` for the final relation, and ``-o FILE`` for file
output. See :doc:`../../cfl/mcfl` for the library API and algorithm details.

CSR: Context-Sensitive Reachability
-----------------------------------

Indexing-based context-sensitive reachability engine for large graphs.

**Binary**: ``csr``  
**Location**: ``tools/cfl/csr/csr.cpp``

CSR operates on graph files (not LLVM bitcode directly) and answers reachability
queries with different indexing strategies (GRAIL, PathTree, or combined).

**Basic Usage**:

.. code-block:: bash

   ./build/bin/csr [options] graph_file

**Common Options** (see ``tools/cfl/csr/README.md`` for full list):

- ``-m <method>`` – Indexing method:

  - ``pathtree`` – PathTree indexing
  - ``grail`` – GRAIL labeling
  - ``pathtree+grail`` – Combined approach

- ``-t`` – Evaluate transitive closure
- ``-r`` – Evaluate tabulation algorithm
- ``-p`` – Evaluate parallel tabulation algorithm
- ``-j <N>`` – Number of threads for parallel tabulation (0 = auto)
- ``-g <file>`` – Generate queries and save to file
- ``-q <file>`` – Load queries from file

**Examples**:

.. code-block:: bash

   # GRAIL-based reachability
   ./build/bin/csr input.graph

   # PathTree indexing
   ./build/bin/csr -m pathtree input.graph

   # Parallel tabulation with 4 threads
   ./build/bin/csr -p -j 4 input.graph
