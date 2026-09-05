Sqid Efficient Relation Chaining
================================

Paper
-----

Lotus implements Chenghang Shi, Haofeng Li, Jie Lu, and Lian Li,
*Context-Free Language Reachability via Efficient Relation Chaining*,
OOPSLA 2026, PACMPL 10, OOPSLA1, Article 162
(`DOI 10.1145/3798270 <https://doi.org/10.1145/3798270>`__).

Problem and relation-chaining view
----------------------------------

For a binary production ``X -> Y Z`` and middle node ``v``, conventional CFL
solving takes the Cartesian product of all incoming ``Y`` edges and outgoing
``Z`` edges at ``v``. The paper calls this operation **relation chaining**.

Two independent choices create avoidable work:

* which side of the product supplies the pivot edges; and
* whether old combinations are reconsidered when a delta arrives.

Sqid combines adaptive chaining and differential chaining over an enhanced
four-view graph representation. It changes the schedule and representation,
not the least fixed point: the resulting labeled relation is identical to the
standard CFL algorithm.

Paper Algorithm 1 is that standard reference algorithm. It corresponds to
Lotus's existing ``SolverSession`` worklist backend and is intentionally
shared rather than reimplemented inside Sqid. The new Sqid contribution is
Algorithms 2-4 below.

Algorithm 2: adaptive chaining
------------------------------

Let ``sources`` be the incoming left-relation endpoints at a middle node and
``targets`` be the outgoing right-relation endpoints. Their Cartesian product
can be enumerated in two equivalent ways:

Backward chaining
   Use each target as a pivot and derive the complete incoming source set.

Forward chaining
   Use each source as a pivot and derive the complete outgoing target set.

Algorithm 2 chooses the side with fewer pivots:

.. code-block:: text

   if |sources| >= |targets|:
       for target in targets: DeriveIn(sources -X-> target)
   else:
       for source in sources: DeriveOut(source -X-> targets)

The number of logical result pairs is unchanged, but fewer derivation calls
and set operations are needed when the endpoint sets are skewed.

Differential chaining
---------------------

For old relations ``Y`` and ``Z`` and deltas ``DeltaY`` and ``DeltaZ``, a
naive incremental product contains four terms. The old-old term is already
known. In a sequential worklist, the delta-delta term also need not be
evaluated independently: whichever delta is processed first becomes old
before the other side is processed.

At each middle node ``v``, Sqid therefore computes only:

.. code-block:: text

   InE(v, Y)      chain DeltaOutE(v, Z)
   DeltaInE(v, Y) chain OutE(v, Z)

The two terms are independent and each invokes adaptive chaining.

Enhanced graph representation
-----------------------------

Every logical edge ``u -X-> v`` has two orientations and two ages:

``InE(v, X)``
   Processed sources of ``X`` edges ending at ``v``.

``OutE(u, X)``
   Processed targets of ``X`` edges beginning at ``u``.

``DeltaInE(v, X)``
   Unprocessed sources in the head view.

``DeltaOutE(u, X)``
   Unprocessed targets in the tail view.

The head and tail views are synchronized at insertion. They have separate
key worklists because the same logical edge participates in different binary
production positions from each orientation.

Algorithm 3: derivation and insertion
-------------------------------------

``DeriveIn`` first subtracts both ``InE`` and ``DeltaInE`` from the proposed
source set. If anything remains, it inserts the difference into
``DeltaInE``, schedules the head key, mirrors each edge into ``DeltaOutE``, and
schedules the corresponding tail keys.

``DeriveOut`` performs the symmetric operation, deduplicating against
``OutE`` and ``DeltaOutE`` before synchronizing the head deltas. This
view-specific duplicate check is important: one view may already have been
flushed while the other remains pending.

Algorithm 4: overall solver
---------------------------

1. Insert terminal edges through ``DeriveIn``.
2. For every nullable symbol and every node, insert its self edge through
   ``DeriveIn``.
3. Drain the incoming-key worklist. Flushing moves one keyed delta from
   ``DeltaInE`` to ``InE``. Apply unary productions, then for every
   ``X -> Y Z`` chain the new incoming ``Y`` set with the processed outgoing
   ``Z`` set.
4. Drain the outgoing-key worklist. Flushing moves one keyed delta from
   ``DeltaOutE`` to ``OutE``. For every ``X -> Z Y``, chain the processed
   incoming ``Z`` set with the new outgoing ``Y`` set.
5. Repeat while either worklist is nonempty.

The incoming loop is deliberately drained before the outgoing loop, matching
the paper. This sequencing is what makes a separate delta-delta product
unnecessary in the single-threaded algorithm.

Lotus implementation
--------------------

Public API
   ``include/CFL/Classical/Solvers/Engines/SQID/SqidEngine.h``

Algorithm
   ``lib/CFL/Classical/Solvers/Engines/SQID/SqidEngine.cpp``

Integration
   ``SolverBackend::Sqid`` and ``--solver sqid`` in the classical, alias, and
   value-flow drivers.

The engine owns all four views, both worklists, flush operations, and adaptive
product selection. The generic Lotus ``Relation`` is only the exact
materialized result exposed to clients; Sqid does not delegate its solving to
the generic worklist backend.

Validation
----------

Tests force both adaptive directions, exercise unary, binary, nullable, cyclic,
and incremental cases, and compare complete labeled relations against an
independent cubic recognizer over generated grammars and graphs. The migrated
POCR alias and value-flow datasets also produce the same start relations as
the baseline solver.
