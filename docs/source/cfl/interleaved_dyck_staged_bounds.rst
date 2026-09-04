Interleaved-Dyck Staged Bounds
==============================

The interleaved-Dyck solver approximates graph reachability constrained by two
independent families of matched delimiters. A path is interleaved-Dyck valid
when projecting its label sequence onto either family produces a valid Dyck
word. The two families may cross in the original path, so ordinary Dyck
reachability over their union is sound but incomplete.

.. warning::

   This is a lower/upper approximation pipeline, not an exact solver for
   general typed interleaved-Dyck reachability.

**Location**: ``include/CFL/InterleavedDyck/StagedBounds/``,
``lib/CFL/InterleavedDyck/StagedBounds/``

The implementation is a native C++17 port of the staged algorithm from
*A Better Approximation for Interleaved Dyck Reachability*. It reuses the
``CFL/InterleavedDyck/MutualRefinement`` CNF saturation engine for reachability and derivation
tracing and consumes the shared graph from ``CFL/InterleavedDyck/Core``.

Relationship to MutualRefinement
--------------------------------

``StagedBounds`` owns the domain-facing analysis policy, while
``MutualRefinement`` supplies the integer-encoded CNF reachability and tracing
engine used by some pipeline stages.

.. list-table:: Responsibility boundary
   :header-rows: 1
   :widths: 24 38 38

   * - Concern
     - StagedBounds
     - MutualRefinement
   * - Input
     - Structured typed delimiter graph
     - Integer ``CnfGraph`` and supplied ``CnfGrammar``
   * - Grammar construction
     - Classic, union-Dyck, parity, endpoint grammars
     - No domain grammar selection
   * - Orchestration
     - Regularization, bounds, condensation, on-demand checks
     - CFL saturation and contributing-edge tracing
   * - Output meaning
     - Interleaved-Dyck lower and upper bounds
     - Grammar-relative reachability edges and derivations

The dependency is one-way. ``MutualRefinement`` has no knowledge of DOT label
conventions, taint/value-flow modes, approximation direction, or the staged
interleaved-Dyck pipeline.

Graph Model
-----------

``lotus::cfl::interleaved_dyck::Graph`` stores signed integer vertex IDs and
deduplicated labeled edges. ``Graph::parseDot`` and
``Graph::parseDotFile`` accept the labels used by the reference datasets:

.. list-table:: Edge labels
   :header-rows: 1
   :widths: 20 80

   * - Label
     - Meaning
   * - ``op--N``
     - Opening parenthesis of type ``N``.
   * - ``cp--N``
     - Closing parenthesis of type ``N``.
   * - ``ob--N``
     - Opening bracket of type ``N``.
   * - ``cb--N``
     - Closing bracket of type ``N``.
   * - ``normal``
     - Neutral edge accepted by both projected grammars.

Staged-Bounds Pipeline
----------------------

``Solver::analyze`` returns an ``ApproximationResult`` containing the
following stages:

``regularization``
   Multiplies the input graph by a benchmark-specific finite automaton and
   runs parenthesis-Dyck reachability on the product. This stage records the
   regular-language filter from the reference artifact.

``intersection``
   Intersects endpoint pairs independently witnessed by the parenthesis and
   bracket projected grammars. The witnesses may be different paths, so this
   is an overapproximation.

``underapproximation``
   Runs ordinary Dyck reachability over the union of both alphabets. Every
   reported path is interleaved-Dyck valid, but valid paths with crossing
   delimiter families may be missed.

``mutual_refinement``
   Condenses vertices that are mutually reachable in the underapproximation.
   It then alternates the two projected analyses, retaining only original
   edges used by successful derivations until the edge set stabilizes.

``stronger_grammar``
   Repeats mutual refinement with a component-local parity and endpoint-state
   grammar. ``Options::parity_groups`` defaults to two, matching the reference
   artifact, and supports values from one through four.

``on_demand``
   Checks remaining unknown endpoint pairs separately, first with the classic
   grammar and then with the stronger grammar. This is the most precise and
   potentially most expensive stage.

The underapproximation is a sound subset of true interleaved-Dyck
reachability. The intersection and refinement stages are overapproximations;
classic refinement, the stronger grammar, and on-demand checking progressively
remove unsupported pairs.

For a queried pair ``(u,v)``:

* membership in ``underapproximation`` means definitely reachable;
* absence from ``on_demand`` means definitely unreachable relative to the
  modeled graph; and
* membership in ``on_demand`` but not ``underapproximation`` remains unknown.

The pipeline happens to be exact on an input when the lower and final upper
bounds coincide. No general equality is assumed.

Using the Solver
----------------

.. code-block:: cpp

   #include "CFL/InterleavedDyck/StagedBounds/Solver.h"

   using namespace lotus::cfl::interleaved_dyck::staged_bounds;

   Graph graph = Graph::parseDotFile("input.dot");

   Options options;
   options.parity_groups = 2;
   options.run_on_demand = true;
   options.factorized_tracing = true; // opt-in lazy provenance reconstruction

   Solver solver;
   ApproximationResult result =
       solver.analyze(graph, BenchmarkKind::Taint, options);

   bool may_reach = result.on_demand.count({source, target}) != 0;
   bool definitely_reaches =
       result.underapproximation.count({source, target}) != 0;

``Options::factorized_tracing`` defaults to ``false``. Setting it to ``true``
switches the refinement stages from eager derivation records to lazy
reconstruction of contributing edges from the saturated CFL relations.

Individual APIs are also available for projected reachability, projected
intersection, the union-Dyck underapproximation, and mutual refinement. Public
pair sets omit trivial self-pairs.

Benchmark Modes
---------------

``BenchmarkKind::Taint``
   Uses the general regularization automaton derived from the bracket labels in
   each graph component.

``BenchmarkKind::ValueFlow``
   Removes vertices outside bracket-source-to-bracket-sink paths, applies the
   value-flow product transformation for the underapproximation, and enforces
   the outer ``ob--0`` / ``cb--0`` source-sink condition.

The reference DOT corpus is stored in
``benchmarks/real-world/CFL/InterleavedDyck/taint`` and
``benchmarks/real-world/CFL/InterleavedDyck/valueflow``. The supplied artifact
did not contain a license file; see the benchmark README for provenance and
redistribution notes.

Build and Test
--------------

The module builds as ``CanaryInterleavedDyckStagedBounds`` and links against
``CanaryInterleavedDyckCore`` and
``CanaryInterleavedDyckMutualRefinement``. Focused tests cover DOT
parsing, crossing delimiters, different-witness rejection, value-flow
preprocessing, the complete staged pipeline, and component-local parity
refinement:

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-staged-bounds
   build/bin/lotus-cfl-interleaved-dyck-staged-bounds input.dot
   cmake --build build --target interleaved_dyck_staged_bounds_test
   ctest --test-dir build -R interleaved_dyck_staged_bounds_test --output-on-failure

The CLI exposes ``--value-flow``, ``--parity-groups N``, ``--no-on-demand``,
``--factorized-tracing``, ``--print-lower``, and ``--print-final``. It
preserves the directed input arcs exactly as parsed.

Cost Considerations
-------------------

The solver materializes all reachable endpoint pairs and derivation traces.
Large dense graphs can therefore require substantial time and memory.
On-demand refinement additionally analyzes unknown pairs one at a time. Set
``Options::run_on_demand`` to ``false`` when the stronger-grammar result is
sufficient and lower latency is more important than the final refinement.

The default eager tracing stores unary and binary derivation records during
saturation, which can dominate memory on dense graphs. Setting
``Options::factorized_tracing`` to ``true`` skips those records and
reconstructs the contributing edges from the saturated relations instead,
trading recomputation time for lower memory use.

See also :doc:`interleaved_dyck_mutual_refinement` and :doc:`interleaved_dyck_graph_reduction`.
