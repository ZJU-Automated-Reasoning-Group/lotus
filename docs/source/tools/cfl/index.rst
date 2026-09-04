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

**Tools**: ``lotus-cfl-classical``, ``lotus-cfl-alias``, ``lotus-cfl-vf``,
``lotus-cfl-foldability``, ``lotus-cfl-pocr``, ``lotus-cfl-staged``,
``lotus-cfl-interleaved-dyck-mcfl``, ``lotus-cfl-interleaved-dyck-mutual-refinement``,
``lotus-cfl-interleaved-dyck-staged-bounds``,
``lotus-cfl-interleaved-dyck-unary``, ``lotus-cfl-interleaved-dyck-graph-reduction``,
and CSR.

Classical CFL and Alias Analysis
--------------------------------

``lotus-cfl-classical`` runs a supplied grammar over a text, DOT, or JSON
graph with the sparse-set, sparse-bitvector, Graspan, transitive-closure, POCR,
hierarchical-POCR, fully ordered, PEARL, or Sqid backend.

``lotus-cfl-alias`` consumes LLVM IR or bitcode. It uses Aser as the constraint
frontend but drives points-to propagation and indirect-call discovery through
the CFL relation. It supports PAG/PEG encodings, annotation validation,
points-to printing, named alias queries, and JSON statistics.

``lotus-cfl-vf`` is the value-flow counterpart. It builds a sparse value-flow
graph from LLVM IR, applies the CFL-specific strong-update preparation, and
answers context-sensitive pointer value-flow queries with matched call/return
labels.

.. code-block:: bash

   cmake --build build --target lotus-cfl-classical lotus-cfl-alias lotus-cfl-vf
   build/bin/lotus-cfl-classical --grammar grammar.txt --graph graph.txt \
     --solver transitive-closure --json-stats
   build/bin/lotus-cfl-alias --encoding pag --solver sparse-bitvector \
     --check-annotations module.bc
   build/bin/lotus-cfl-vf --solver transitive-closure \
     --query main::source,main::sink module.bc

Use ``--solver pocr``, ``--solver hpocr``, or ``--solver focr`` to select the
ported POCR algorithm families. The same selectors are available to the alias
and value-flow clients.

The hand-specialized engines are separate from those general grammar
backends. Use ``lotus-cfl-alias --engine pocr-aa|focr-aa --encoding peg`` or
``lotus-cfl-vf --engine pocr-vfa|focr-vfa``. ``Clients/`` still contains only
the alias and value-flow adapters; the implementations live under
``Solvers/Engines/``.

``lotus-cfl-pocr`` drives the standard, Graspan, grammar-rewritten,
rewritten-Graspan, POCR, and FOCR engine choices directly on POCR ``.peg`` and
``.vfg`` files for artifact comparison, without creating another client layer.
It exposes POCR's SCC, graph-folding, InterDyck-pruning, graph-output, and
optional ECG-SCC controls directly.

``lotus-cfl-foldability`` ports POCR's recursive-state-machine-guided
foldability checker. The general driver also accepts POCR grammar/graph files
directly and exposes unidirectional summarization, SCC elimination, graph
folding, and inter-Dyck pruning.

``lotus-cfl-staged`` runs the ISSTA 2024 Stg solver. It accepts explicit
standard-Dyck, extended-Dyck, or Alias-CFP decomposition parameters and DNF
regular productions for Phase L and Phase R.

See :doc:`../../cfl/classical`, :doc:`../../cfl/pearl`,
:doc:`../../cfl/stg`, and :doc:`../../cfl/sqid` for the complete algorithm,
option, and API descriptions.

MCFL: Multiple Context-Free Language Reachability
-------------------------------------------------

Runs the POPL 2025 MCFL hierarchy for underapproximating interleaved-Dyck
reachability on artifact-compatible DOT graphs.

**Binary**: ``lotus-cfl-interleaved-dyck-mcfl``

**Location**: ``tools/cfl/interleaved-dyck/mcfl/lotus-cfl-interleaved-dyck-mcfl.cpp``

.. code-block:: bash

   cmake -S . -B build -DLOTUS_ENABLE_CFL=ON
   cmake --build build --target lotus-cfl-interleaved-dyck-mcfl
   ./build/bin/lotus-cfl-interleaved-dyck-mcfl --dimension 2 input.dot

Useful options include ``--simple`` for the weaker ``G_d^circ`` grammar,
``--no-condense`` to disable cycle elimination, ``--stats`` for saturation
counters, ``--artifact-compatible`` for exact condensed cross-product
expansion, ``--print-pairs`` for the final relation, and ``-o FILE`` for file
output. See :doc:`../../cfl/interleaved_dyck_mcfl` for the library API and
algorithm details.

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

Mutual Refinement of CFL Reachability
-------------------------------------

Runs the SAS 2023 mutual-refinement algorithm over a grammar file and a DOT
graph. The grammar file holds one or more ``{ ... }`` blocks whose first ``|``
row names the start symbol and whose later rows encode epsilon, unary, or
binary productions.

**Binary**: ``lotus-cfl-interleaved-dyck-mutual-refinement``

**Location**: ``tools/cfl/interleaved-dyck/mutual-refinement/lotus-cfl-interleaved-dyck-mutual-refinement.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-mutual-refinement
   build/bin/lotus-cfl-interleaved-dyck-mutual-refinement grammars.txt graph.dot refine

The final argument selects ``naive`` (independent CFL saturation per grammar,
then intersection) or ``refine`` (alternating refinement loop). Pass
``--factorized-tracing`` after ``refine`` to reconstruct contributing edges
from the saturated relations instead of eager derivation records. See
:doc:`../../cfl/interleaved_dyck_mutual_refinement` for the library API and
algorithm details.

Interleaved-Dyck Staged Bounds
------------------------------

Computes staged lower and upper bounds for typed interleaved-Dyck reachability
on a DOT graph: a certified lower bound, then progressively tighter
overapproximations through parity refinement, mutual refinement, and on-demand
checks.

**Binary**: ``lotus-cfl-interleaved-dyck-staged-bounds``

**Location**: ``tools/cfl/interleaved-dyck/staged-bounds/lotus-cfl-interleaved-dyck-staged-bounds.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-staged-bounds
   build/bin/lotus-cfl-interleaved-dyck-staged-bounds --parity-groups 2 \
     --factorized-tracing graph.dot

Useful options include ``--value-flow`` for value-flow benchmark
preprocessing, ``--no-on-demand`` to stop after the stronger grammar,
``--print-lower``/``--print-final`` for the certified lower or final upper
pairs, and ``-o FILE`` for file output. See
:doc:`../../cfl/interleaved_dyck_staged_bounds` for the library API and
algorithm details.

Unary Interleaved-Dyck Reachability
-----------------------------------

Computes exact bidirected unary ``D1``-interleaved-``D1`` reachability on a DOT
graph with the adaptive (default) or fixed-counter algorithm.

**Binary**: ``lotus-cfl-interleaved-dyck-unary``

**Location**: ``tools/cfl/interleaved-dyck/unary/lotus-cfl-interleaved-dyck-unary.cpp``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-unary
   build/bin/lotus-cfl-interleaved-dyck-unary --algorithm adaptive graph.dot

Useful options include ``--direct`` to skip quotient sparsification,
``--bidirect`` to add missing complement reverse arcs (a sound
overapproximation of the original directed graph), ``--shallow K`` for the
adaptive-only shallow solve, ``--stats`` for construction and backend
statistics, and ``--print-pairs`` to materialize non-reflexive component
pairs. See :doc:`../../cfl/interleaved_dyck_unary` for the library API and
algorithm details.

Interleaved-Dyck Graph Reduction
--------------------------------

Implements the PLDI 2020 graph-simplification pipeline for interleaved-Dyck
reachability. It is a graph transformation, not a reachability solver: it
edits a working copy of a DOT graph in place and produces a smaller graph that
preserves the reachability property covered by the reduction theorem.

**Binary**: ``lotus-cfl-interleaved-dyck-graph-reduction`` (Python driver) with the
compiled helpers ``lotus-cfl-interleaved-dyck-graphaux`` and
``lotus-cfl-interleaved-dyck-dkmerge``

**Location**: ``tools/cfl/interleaved-dyck/graph-reduction/``

.. code-block:: bash

   cmake --build build --target lotus-cfl-interleaved-dyck-graph-reduction
   cp input.dot reduced.dot
   python3 build/bin/lotus-cfl-interleaved-dyck-graph-reduction.py reduced.dot \
     --graphaux build/bin/lotus-cfl-interleaved-dyck-graphaux \
     --dkmerge build/bin/lotus-cfl-interleaved-dyck-dkmerge

``lotus-cfl-interleaved-dyck-graphaux`` performs one-color component construction
(``lotus-cfl-interleaved-dyck-graphaux <graph.dot>``) and
``lotus-cfl-interleaved-dyck-dkmerge`` performs the degree-based merge phase; the
Python driver alternates both colors and removes proven-redundant edges. Pass
``--bidirected-input`` when the input already represents both directions. See
:doc:`../../cfl/interleaved_dyck_graph_reduction` for the library API and algorithm
details.
