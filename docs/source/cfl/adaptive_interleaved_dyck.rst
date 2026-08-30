Exact Adaptive Interleaved Dyck
===============================

``AdaptiveInterleavedDyck`` computes the exact all-pairs component partition
for bidirected unary ``D1``-interleaved-``D1`` graphs using adaptive counter
flattening.

**Location**: ``include/CFL/AdaptiveInterleavedDyck/``,
``lib/CFL/AdaptiveInterleavedDyck/``

The module consumes ``lotus::cfl::interleaved_dyck::Graph`` from
``InterleavedDyckCore``. All parenthesis IDs project to counter 1 and all
bracket IDs project to counter 2. Every projected arc must have its
complement-labeled reverse.

Directed-input policy
---------------------

``AdaptiveInterleavedOptions::input_policy`` has two modes:

``RequireBidirected``
   Reject a missing complement reverse arc and remain exact for the original
   graph.

``AddMissingReverseEdges``
   Symmetrize the graph. The result is exact for the symmetrized graph and a
   sound overapproximation for the original directed graph. Statistics report
   inserted arcs and the changed guarantee.

The shared parser itself never adds reverse arcs.

APIs
----

``AdaptiveInterleavedDyckSolver::solveShallow(graph, K)`` computes exactly the
zero-configuration partition inside ``min(counter1, counter2) <= K``.

``AdaptiveInterleavedDyckSolver::solve(graph)`` applies quotient
sparsification, chooses ``K = 6 * |V(quotient)|``, runs both one-counter arms,
merges their boundary partitions, and lifts identifiers to the input graph.

.. code-block:: cpp

   #include "CFL/AdaptiveInterleavedDyck/AdaptiveInterleavedDyck.h"
   #include "CFL/InterleavedDyckCore/Graph.h"

   using namespace lotus::cfl;

   interleaved_dyck::Graph graph =
       interleaved_dyck::Graph::parseDotFile("bidirected.dot");
   auto result =
       adaptive_interleaved_dyck::AdaptiveInterleavedDyckSolver{}.solve(graph);
   bool reachable = result.connected(source, target);

Benchmark eligibility
---------------------

All interleaved-Dyck algorithms can load the same DOT files through the shared
core. The existing ``benchmarks/interleaved-dyck-approximation`` corpus is
directed, however, so the adaptive exact solver rejects it. Comparative
experiments need a genuinely bidirected corpus or an explicitly documented
bidirecting transformation. The standalone CLI exposes the latter as
``--bidirect`` and always prints the resulting guarantee.

Build and Test
--------------

.. code-block:: console

   cmake --build build --target lotus-cfl-adaptive-interleaved-dyck
   build/bin/lotus-cfl-adaptive-interleaved-dyck --stats bidirected.dot
   cmake --build build --target adaptive_interleaved_dyck_test
   ctest --test-dir build -R adaptive_interleaved_dyck_test --output-on-failure
