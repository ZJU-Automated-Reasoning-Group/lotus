Exact Unary Interleaved Dyck
============================

The ``Unary`` module contains exact algorithms for bidirected unary
``D1``-interleaved-``D1`` reachability. The module name identifies the problem
scope; ``AdaptiveSolver`` and ``FixedCounterSolver`` identify the algorithms.

**Location**: ``include/CFL/InterleavedDyck/Unary/``,
``lib/CFL/InterleavedDyck/Unary/``

Algorithms
----------

``AdaptiveSolver``
   Uses the adaptive two-arm construction. ``solve`` applies the shared
   quotient, chooses ``K = 6 * |V(quotient)|``, and uses quadratic finite
   control. ``solveShallow(graph, K)`` solves exactly inside
   ``min(counter1, counter2) <= K``.

``FixedCounterSolver``
   Implements Kjelstrøm and Pavlogiannis, POPL 2022 Algorithm 1. For ``n``
   processed vertices it stores one counter in states ``(v,j)`` for
   ``0 <= j <= 18*n^2 + 6*n`` and leaves the other as a bidirected
   one-counter problem. It is the primary exact baseline for Adaptive.

Both algorithms share typed graph parsing, unary projection, input policy,
quotient sparsification, and the bidirected-Dyck component backend.

Exactness and directed input
----------------------------

Every projected arc must have its complement-labeled reverse. Parenthesis IDs
project to counter 1 and bracket IDs to counter 2. Exactness concerns balanced
paths between zero-counter configurations; it does not extend to multi-type
``D_k``-interleaved-``D_k``.

Both option types default to ``RequireBidirected``. The alternative
``AddMissingReverseEdges`` is exact for the resulting symmetrized graph and a
sound overapproximation for the original directed graph. The parser itself
never adds reverse edges.

.. code-block:: cpp

   #include "CFL/InterleavedDyck/Core/Graph.h"
   #include "CFL/InterleavedDyck/Unary/Solver.h"

   using namespace lotus::cfl;

   interleaved_dyck::Graph graph =
       interleaved_dyck::Graph::parseDotFile("bidirected.dot");
   auto adaptive = interleaved_dyck::unary::AdaptiveSolver{}.solve(graph);
   auto fixed = interleaved_dyck::unary::FixedCounterSolver{}.solve(graph);

Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-unary
   build/bin/lotus-cfl-interleaved-dyck-unary --algorithm adaptive --stats bidirected.dot
   build/bin/lotus-cfl-interleaved-dyck-unary --algorithm fixed-counter --stats bidirected.dot

``--direct`` disables shared quotient sparsification. ``--shallow K`` applies
only to Adaptive. ``--bidirect`` explicitly selects symmetrization, and output
states the selected algorithm and resulting guarantee.

The existing ``benchmarks/real-world/CFL/InterleavedDyck`` corpus is directed,
so exact runs require a genuinely bidirected corpus. Using ``--bidirect`` on
that corpus must be reported as an overapproximate experiment.

Build and test
--------------

.. code-block:: console

   cmake --build build --target interleaved_dyck_unary_test
   ctest --test-dir build -R interleaved_dyck_unary_test --output-on-failure
