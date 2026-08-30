Multiple Context-Free Language Reachability
===========================================

The ``CFL/MCFL`` component contains two related but semantically distinct
solvers: exact reachability for a client-supplied MCFG and dimension-indexed
typed interleaved-Dyck underapproximations.

**Location**: ``include/CFL/MCFL/``, ``lib/CFL/MCFL/``

Choosing an MCFL API
--------------------

.. list-table:: Guarantees of the two public solver APIs
   :header-rows: 1
   :widths: 24 31 25 20

   * - API
     - Model
     - Guarantee
     - Output
   * - ``mcfl::Solver``
     - A client-supplied supported MCFG
     - Exact for that grammar
     - Pair relation, tuple facts, witnesses
   * - ``mcfl::InterleavedDyckSolver``
     - Typed ``G_d^circ`` or ``G_d^+`` grammar
     - Sound underapproximation
     - Pair set and statistics per dimension
Exactness of ``mcfl::Solver`` is relative to its input grammar. When the input
grammar is ``G_d^circ`` or ``G_d^+``, the result remains an
underapproximation of typed interleaved-Dyck reachability. Exact unary
reachability lives in the independent :doc:`unary_interleaved_dyck` module.

Generic MCFL Solver
-------------------

``lotus::cfl::mcfl::Grammar`` represents a non-deleting, non-permuting MCFG in
the paper's normal form. Its builders cover the five normal-form operations:
terminal/epsilon seeds, terminal prepend, terminal append, independent
component insertion, and component concatenation. ``Grammar::validate``
checks all arities, variable references, linearity, non-deletion, and variable
order before analysis starts.

``Solver::solve`` computes all derived tuples
``A[(u1,v1),...,(uk,vk)]`` with worklist saturation. Type-5 joins are indexed
by individual component endpoints. Adjacent tuple components that cannot be
connected in the underlying graph are pruned, as described in the paper's
implementation section.

The result exposes:

``reachablePairs``
   All start-symbol endpoint pairs, including reflexive epsilon results.

``facts``
   The complete saturated relation for every nonterminal and tuple component.

``stats``
   Fact, worklist, join, duplicate, and pruning counters.

``witness(u,v)``
   A concrete labeled path reconstructed from the first proof DAG for the
   requested reachable pair.

.. code-block:: cpp

   #include "CFL/MCFL/Grammar.h"
   #include "CFL/MCFL/Graph.h"
   #include "CFL/MCFL/Solver.h"

   using namespace lotus::cfl::mcfl;

   Grammar grammar;
   auto s = grammar.addNonterminal("S", 1);
   auto atom = grammar.addNonterminal("Atom", 1);
   auto tail = grammar.addNonterminal("Tail", 1);
   grammar.setStart(s);
   grammar.addBasic(atom, "x");
   grammar.addAppend(tail, atom, "b", 0);
   grammar.addPrepend(s, tail, "a", 0);

   Graph graph;
   graph.addEdge(0, 1, "a");
   graph.addEdge(1, 2, "x");
   graph.addEdge(2, 3, "b");

   ReachabilityResult result = Solver{}.solve(graph, grammar);
   bool reachable = result.reaches(0, 3);
   auto path = result.witness(0, 3);

Interleaved-Dyck Underapproximation
----------------------------------

``buildInterleavedDyckGrammar`` creates either of the paper's rank-2 grammar
families for any positive dimension:

``InterleavedGrammarVariant::Simple``
   The bounded component-interleaving grammar ``G_d^circ``.

``InterleavedGrammarVariant::Full``
   The stronger ``G_d^+`` grammar with insertion and nesting productions.

The staged ``InterleavedDyckSolver`` accepts artifact-compatible DOT labels:

.. list-table:: Edge labels
   :header-rows: 1
   :widths: 20 80

   * - Label
     - Meaning
   * - ``op--N`` / ``cp--N``
     - Opening/closing parenthesis of type ``N``.
   * - ``ob--N`` / ``cb--N``
     - Opening/closing bracket of type ``N``.
   * - ``normal``
     - Neutral edge accepted by both projections.

For dimensions ``1`` through ``d``, the driver filters unmatched delimiter
types, performs the artifact's neutral-edge and mutual-reachability
condensation, splits weak components, computes projected Dyck feasibility,
runs MCFL saturation, and expands pairs to the original graph. Staged results
omit reflexive pairs.

Every reported pair is certified typed interleaved-Dyck reachable. A missing
pair is unresolved rather than certified unreachable: it may require a larger
dimension or a path outside the selected grammar family.

The default expansion filters the Cartesian product of condensed vertices by
plain reachability in the original graph. This avoids a reference-artifact
quirk where contracting a one-way ``normal`` edge reports its reverse pair.
Set ``CondensationExpansionPolicy::ArtifactCompatible`` for exact reproduction
of that cross-product behavior.

Shared typed graph adapter
--------------------------

``InterleavedDyckSolver`` accepts
``lotus::cfl::interleaved_dyck::Graph`` directly. The adapter converts typed
labels to the generic MCFL terminal strings, allowing MCFL and
:doc:`interleaved_dyck_approximation` to consume one parsed benchmark graph.
The generic ``mcfl::Graph`` remains available for arbitrary client grammars
whose terminals are not interleaved-Dyck labels.

Command-Line Tool
-----------------

Build CFL tools and run the full hierarchy through dimension two:

.. code-block:: console

   cmake -S . -B build -DLOTUS_ENABLE_CFL=ON
   cmake --build build --target lotus-cfl-mcfl
   build/bin/lotus-cfl-mcfl --dimension 2 graph.dot

``--simple`` selects ``G_d^circ``. ``--no-condense`` disables cycle
elimination, ``--artifact-compatible`` selects the artifact's condensed
cross-product expansion, ``--stats`` prints saturation counters, and
``--print-pairs`` emits the final endpoint relation.

The exact unary algorithms share the separate
``lotus-cfl-unary-interleaved-dyck`` executable documented in
:doc:`unary_interleaved_dyck`.

Validation and Complexity
-------------------------

Unit tests cover all five rule types, empty output components, rank-three
joins, epsilon semantics, two-dimensional copy languages, proof witnesses,
grammar validation, DOT parsing, and the paper's length-4 and length-6
language-coverage counts. Artifact benchmark regression checks load the shared
typed graph and reproduce the published MCFL pair counts.

For fixed grammar dimension ``d`` and rank ``r``, the theoretical bounds are
those proved in the paper: polynomial grammar factors times
``delta * n^(2d)`` for rank one, and polynomial grammar factors times
``n^(d(r+1))`` for rank greater than one. The solver materializes all derived
facts and proofs, so dense high-dimensional instances can require substantial
time and memory.

The implementation is clean-room C++ based on the published algorithm. The
GPLv3 reference artifact is used only for observable compatibility checks; its
Go/Python source is not copied into Lotus.
