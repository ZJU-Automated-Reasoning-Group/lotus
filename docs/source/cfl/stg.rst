Stg Staged Solving
==================

Paper
-----

Lotus implements Chenghang Shi, Haofeng Li, Jie Lu, and Lian Li,
*Better Not Together: Staged Solving for Context-Free Language Reachability*,
ISSTA 2024 (`DOI 10.1145/3650212.3680346
<https://doi.org/10.1145/3650212.3680346>`__).

Problem and key idea
--------------------

Many program-analysis grammars combine two different problems:

* a small context-free part that matches paired program actions, such as
  calls and returns, field writes and reads, or address and dereference
  operations; and
* a much larger regular part that propagates ordinary control or data flow.

A monolithic CFL solver repeatedly mixes both parts. Stg instead identifies a
**context-free pattern** (CFP), decomposes the grammar into a context-free
grammar ``L`` and a regular grammar ``R``, and solves them in order:

``Phase L``
   Derive only CFP summary edges using a pattern-specific solver.

``Phase R``
   Treat those summaries as fixed labels and solve the remaining regular
   expressions using ordered propagation.

The expensive context-free computation consequently operates on fewer edges,
while regular reachability can exploit graph order that would not be stable
during monolithic saturation.

Context-free patterns and decomposition
---------------------------------------

The paper defines a CFP as ``S' -> a E b``. ``a`` and ``b`` are matched
terminal actions, ``E`` is a regular expression over terminals and grammar
variables, and ``S'`` names the summary relation.

CFP-based decomposition performs four conceptual steps:

1. Introduce ``S' -> a Ec b`` for each concrete CFP instance.
2. In the remaining productions ``PR``, replace occurrences of the concrete
   matched pattern with ``S'`` and rewrite every right-hand side as a regular
   expression over terminals and summary symbols.
3. Substitute those regular expressions into the body of the ``S'``
   production, removing dependencies from ``L`` back to variables in ``R``.
4. Solve ``L = ({S'}, Sigma, PL, S')`` first and the regular grammar
   ``R = (N - {S'}, Sigma union {S'}, PR, S)`` second.

The choice of CFP and the concrete rewritten equations is analysis knowledge.
Lotus therefore represents the decomposition explicitly with
``StagedSpecification`` rather than guessing patterns from symbol names. The
library supports several Dyck and Alias CFP entries in one specification.

Dyck CFP
--------

For matched delimiter families ``Li`` and ``Mi`` and neutral labels ``s``,
the Phase-L summary is:

.. code-block:: text

   Sum -> Li (Sum | s)* Mi

The solver maintains the transitive closure of the body relation
``(Sum | s)*``. Each new body path is joined with an incoming ``Li`` edge and
the corresponding outgoing ``Mi`` edge. A newly produced ``Sum`` edge is
inserted into the same body closure, which discovers nesting. Lotus reuses its
incremental transitive-closure utility for this tabulation step instead of
duplicating an equivalent closure implementation.

The supplied decomposition helpers implement both instances from the paper:

Standard Dyck
   ``S -> (Sum | s)*`` after Phase L.

Extended Dyck
   ``Start -> (Sum | s | Mi)* (Sum | s | Li)*``. The first expression permits
   unmatched closes before a balanced region; the second permits unmatched
   opens after it.

Matched delimiter attributes are preserved: ``Li`` is paired only with the
``Mi`` having the same concrete attribute.

Alias CFP and Algorithm 1
-------------------------

The Alias CFP has the form ``X -> a A* Y B* b``. To avoid materializing every
possible ``A* Y B*`` path, Algorithm 1 keeps only paths that can contribute to
a matched summary. With the paper's reverse relation written as ``Abar``, the
Lotus specification is:

.. code-block:: text

   X -> open Abar* Y B* close

The two path relations are:

Forward path
   ``u -> v`` represents a useful ``Y B*`` path. Every ``Y`` edge seeds the
   forward worklist. A forward path extends over outgoing ``B`` edges.

Backward path
   Once a forward path reaches a node with a ``close`` edge, the algorithm
   reverses that path into the backward worklist. A backward path extends over
   outgoing ``Abar`` edges, which corresponds to walking backward through the
   original ``A`` relation.

When a backward path reaches a node with an incoming ``open`` edge, every
compatible ``close`` successor produces an ``X`` summary edge.

The feedback block at lines 20-29 of Algorithm 1 is essential. A new ``X``
edge may create new ``Abar``, ``Y``, or ``B`` edges through the rewritten
Phase-L equations. Lotus repeats Phase L to a fixed point and handles them as
follows:

* new ``Y`` edges seed new forward paths;
* new ``B`` edges extend all forward paths that end at their source; and
* new ``Abar`` edges extend all backward paths that end at their source.

This is a dedicated Alias-CFP engine, not a generic grammar substitution.

Regular Phase R and Algorithm 2
-------------------------------

Each production in ``R`` is normalized to disjunctive normal form. Every
alternative is a sequence ``e1 e2 ... em`` whose atoms are a symbol or a
Kleene closure over a union of symbols. Alternatives are evaluated separately
and their output relations are united.

For one sequence, Algorithm 2 maintains ``Rold(v)``, the sources reaching
``v`` before the current atom, and ``R(v)``, the sources reaching ``v`` after
it:

Literal atom ``a``
   Clear ``R`` and, for every ``u -a-> v``, union ``Rold(u)`` into ``R(v)``.

Kleene atom ``a*``
   Start with ``R = Rold`` for the epsilon case. Build the subgraph containing
   only ``a`` edges, collapse its strongly connected components, retain one
   source set per component, and propagate the sets once in topological order.

After each atom, ``R`` becomes ``Rold`` for the next iteration. Because all
Phase-L summaries are fixed before Phase R begins, the SCC order remains
valid. For sparse graphs the paper characterizes this phase as close to
quadratic, with complexity ``O(m n)``.

Nested closures such as ``(a b*)*`` use the paper's auxiliary-nonterminal
construction: first materialize ``C -> a b*``, then solve ``C*``.

Lotus implementation
--------------------

Public API
   ``include/CFL/Classical/Solvers/Engines/STG/StagedSolver.h``

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/STG/StagedSolver.cpp``

Driver
   ``lotus-cfl-staged`` with ``standard-dyck``, ``extended-dyck``, and
   ``alias`` modes. Repeated ``--phase-l`` and ``--phase-r`` definitions form
   DNF alternatives.

Stg is separate from ``SolverBackend`` because a backend enum and an arbitrary
CFG do not contain the CFP/decomposition information required by the paper.

Validation
----------

Tests compare standard Dyck, extended Dyck, and Alias CFP results with
monolithic CFL solving. Generated graph tests exercise nesting, multiple
delimiter families, cycles, and forward/backward Alias paths. Focused tests
cover Algorithm 1's new-summary feedback for all three ``Abar``/``Y``/``B``
relations, SCC-ordered Algorithm 2 propagation, DNF unions, and the
auxiliary-nonterminal treatment of nested Kleene expressions.
