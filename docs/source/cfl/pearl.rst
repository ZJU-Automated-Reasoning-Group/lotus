PEARL Multi-Derivation
======================

Paper
-----

Lotus implements the algorithm from Chenghang Shi, Haofeng Li, Yulei Sui,
Jie Lu, Lian Li, and Jingling Xue, *Two Birds with One Stone:
Multi-Derivation for Fast Context-Free Language Reachability Analysis*,
ASE 2023 (`DOI 10.1109/ASE56229.2023.00118
<https://doi.org/10.1109/ASE56229.2023.00118>`__).

The implementation is independent of the paper artifact's SVF utilities. It
uses the Lotus ``Grammar``, ``Relation``, and sparse-bitvector containers.

Problem and key idea
--------------------

The conventional CFL worklist derives one labeled edge at a time. For a
partially transitive production such as ``X -> X A``, many ``X`` facts ending
at the same node are consequently propagated along the same ``A`` edge in
separate operations. Fully transitive rules such as ``A -> A A`` also create
secondary ``A`` edges that repeat reachability already implied by an
``A``-path.

PEARL addresses both forms of redundancy:

* **relation packing** groups the sources of many ``X`` edges ending at one
  node into a set and propagates that set in one operation; and
* **primary-edge propagation graphs** retain a sparse skeleton for each fully
  transitive relation. Reachability is propagated through that skeleton, so
  derived secondary edges do not trigger the same transitive work again.

For a symbol ``X`` and node ``v``, the paper writes the packed predecessor
relation as ``R(X, v) = {u | u -X-> v}``. A production ``X -> X A`` therefore
induces a set constraint ``R(X, u) subseteq R(X, v)`` for every ``A`` edge
``u -A-> v``.

Production classification
-------------------------

PEARL assumes the weak-Chomsky-normal-form grammar used by classical CFL
reachability. Lotus's canonical grammar frontend performs that normalization.
Binary rules are classified as follows:

``A -> A A``
   A fully transitive production. ``A`` is maintained using ``PG(A)``.

``X -> X A``
   A left-partially-transitive production when ``A`` is fully transitive.

``X -> A X``
   A right-partially-transitive production when ``A`` is fully transitive.
   The paper handles this by reversing the relation. Lotus implements the
   symmetric propagation directly and also supports explicit ``X``/``Xbar``
   pairs when a client uses the paper's inverse-relation representation.

All other unary and binary rules
   Non-transitive rules handled by ordinary indexed worklist joins.

Paper Algorithm 1 is the standard CFL-reachability baseline. Lotus already
implements the same nullable, unary, and binary saturation in
``SolverSession``. PEARL's non-transitive phase applies the same indexed join
semantics to ``CFGnt``; the paper's reference algorithm is not exposed as a
redundant second baseline backend.

Algorithm 2: overall solver
---------------------------

The implementation preserves the paper's three kinds of pending work:

1. Initialize terminal facts and per-node facts for nullable symbols.
2. Remove full and partial transitive productions from ``CFGnt``.
3. Saturate ``CFGnt`` using the standard worklist algorithm.
4. Pack newly produced partial-transitive facts and run Algorithm 3.
5. Insert newly produced full-transitive facts into propagation graphs and
   run Algorithm 4.
6. Return transitive facts to the ordinary worklist because they may enable
   non-transitive productions. Repeat until all three work classes are empty.

Lotus realizes these roles as the non-transitive edge worklist, the keyed
partial-relation worklist, and the full-primary-edge worklist. Facts remain in
one exact public ``Relation`` throughout the fixed-point computation.

Algorithm 3: partially transitive relations
-------------------------------------------

For every partial symbol and endpoint, the engine maintains an old packed set
and a delta packed set. Processing follows the paper's ``PackRR``,
``PropRRs``, and ``DiffProp`` operations:

1. ``PackRR`` inserts a newly derived source into the endpoint's delta set.
   For an explicit inverse pair, it also packs the reversed fact.
2. A keyed node worklist ensures that multiple insertions are combined before
   propagation.
3. On a pop, the delta is removed from the new set and merged into the old
   set.
4. The complete delta set is propagated along every applicable outgoing
   primary edge in ``PG(A)``. Right-partial rules use the symmetric incoming
   primary-edge traversal.
5. Only set differences absent from the destination's old relation are
   scheduled again.

One ``X`` relation may participate in several rules, such as ``X -> X A1``
and ``X -> X A2``. The same packed delta is propagated through every
applicable propagation graph before it is discarded.

Algorithm 4: fully transitive relations
---------------------------------------

For a candidate full-transitive edge ``u -A-> v``:

1. Reject it as a primary edge if ``u`` already reaches ``v`` in ``PG(A)``.
2. Otherwise add it to ``PG(A)`` and, when configured, add the reversed
   primary edge to ``PG(Abar)``.
3. Immediately propagate existing partial relations at ``u`` across the new
   primary edge.
4. Form ``srcSet`` from ``u`` and all existing predecessors of ``u``.
5. Run difference DFS from ``v`` through primary successors. At each visited
   node, only sources not already present in the full relation continue.
6. Materialize every newly discovered full ``A`` fact in the public relation,
   but never insert secondary closure facts into the primary graph.

The paper notes that insertion order can leave an edge that later becomes
redundant in the primary graph. Lotus follows that online rule rather than
attempting a more expensive dynamic transitive reduction.

Full-and-partial corner case
----------------------------

A symbol can have both ``X -> X X`` and ``X -> X A``. The paper gives two
ways to restore completeness; Lotus implements **Option 2**:

* an ``X`` fact produced by partial propagation is offered to ``PG(X)`` as a
  new full-transitive primary candidate; and
* an ``X`` fact produced by full closure is packed into the partial relation
  and propagated through ``PG(A)``.

This cross-feeding continues until neither worklist changes.

Lotus implementation
--------------------

Public API
   ``include/CFL/Classical/Solvers/Engines/PEARL/PearlEngine.h``

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/PEARL/PearlEngine.cpp``

General integration
   ``SolverBackend::Pearl`` in ``SolverSession`` and ``--solver pearl`` in the
   classical, alias, and value-flow drivers.

Inverse relations
   ``SolverOptions::pearl_inverse_relations`` or repeated
   ``--pearl-inverse X,XBAR`` options in ``lotus-cfl-classical``. Pairing is
   explicit; symbol names are never guessed.

The engine is not a wrapper around the generic solver. It owns the packed
relations, propagation graphs, difference propagation, and PEARL worklists.

Validation
----------

The Classical CFL tests cover left and right partial rules, several guards
for one packed relation, primary versus secondary full edges, explicit inverse
packing, inherited transitivity of a paired inverse relation, the
full-and-partial Option 2 case, nullable initialization, incremental solving,
and generated problems compared with the independent cubic recognizer.
