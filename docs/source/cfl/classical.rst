Classical Grammar-Driven CFL Reachability
==========================================

``CFL/Classical`` is the general grammar-driven part of Lotus's CFL
subsystem. It is independent of the specialized interleaved-Dyck and MCFL
solver representations.

Architecture
------------

The public and implementation trees mirror the same responsibility-based
layout:

``Core/``
   Canonical grammar, labeled graph, relation storage, and validation.

``Solvers/``
   Worklist reachability, specialized transitive closure, and the separate
   constraint-grounding analysis.

``Clients/Alias/``
   PAG/PEG encoding, Aser synchronization, and the LLVM alias facade.

``Clients/ValueFlow/``
   SVFG preparation, encoding, and context-sensitive value-flow queries.

``Grammar``
   Parses declared start, terminal, and nonterminal symbols; normalizes
   whitespace-delimited ``*`` and ``?`` EBNF; binarizes long productions; and
   compiles string symbols to stable integer IDs. ``GrammarParseOptions``
   instantiates correlated attributed symbols such as ``call_i``/``ret_i``
   using variable-specific or per-symbol domains. Graph labels provide domains
   automatically when the command-line driver is used. Only ``<epsilon>`` is
   reserved for epsilon; ``e`` and ``epsilon`` are ordinary terminals. A
   configurable expansion limit rejects independent attribute domains whose
   Cartesian product would grow unexpectedly large. The former independent
   ``CNFGrammar`` parser/transformer has been removed; this is the only grammar
   normalization implementation.

``LabeledGraph``
   Stores the base problem boundary. Solver sessions keep derived facts in a
   separate relation; only explicit incremental terminal additions modify the
   graph. Text, DOT, and JSON readers are available; DOT accepts quoted IDs and
   JSON has an explicit ``nodes``/``vertices`` section for isolated vertices.
   Plain, reverse, and bidirectional transformations are explicit. Forward and
   reverse label indices support direct incoming-edge queries.

``Relation``
   Separates terminal and derived facts from the graph frontend. Sparse-set
   and LLVM sparse-bitvector implementations provide visitor-based indexed
   successor and predecessor lookup without materializing vectors in joins.

``SolverSession``
   Retains a relation and worklist across calls. ``addTerminalEdge`` followed
   by ``solve`` supports clients that discover constraints incrementally.
   Nullable self-facts are seeded only for newly added nodes.

Solver backends
---------------

``SparseSet``
   Conventional indexed worklist saturation with hash-set relations.

``SparseBitVector``
   The same worklist algorithm with per-node, per-symbol LLVM sparse
   bitvectors. This is a storage choice, not the POCR algorithm.

``TransitiveClosure``
   Uses sparse bitvectors generally and a dedicated incremental forward/reverse
   bitvector closure for every production ``X -> X X``. Inserting ``u -> v``
   crosses predecessors of ``u`` with successors of ``v``. It does not retain
   copied reachability trees or a second hash-set closure.

All backends return exactly the same grammar-relative relation. Tests compare
their complete triples, not only start-symbol answers.

``ConstraintGroundingSolver`` is separate: it computes structural set-variable
grounding statistics and does not expose a CFL node-pair relation.

Adapters
--------

Adapter implementations are split by dependency:

``CanaryClassicalCFLAlias``
   PAG and PEG encodings, ``AliasClient``, and ``solveToFixedPoint`` for
   alternating client-defined discovery with incremental saturation. PEG
   loads and stores added after solving are converted through existing or
   reusable synthetic dereference nodes.

``AserConstraintAdapter.h``
   A header-only converter from Lotus's native AserPTA constraint graph to the
   alias client input. A client-provided offset resolver preserves field GEP
   attributes, while ``AserAliasSynchronizer`` maps new Aser nodes and
   constraints into successive ``solveToFixedPoint`` rounds, including when
   PEG has inserted synthetic nodes.

``LLVMCFLAliasAnalysis`` / ``lotus-cfl-alias``
   Build Aser's constraint/model frontend without running its points-to
   solver. CFL points-to facts resolve indirect and intercepted calls; normal
   Aser callbacks then create actual/formal, return, heap, and newly reached
   function constraints. Constant global initializers and pointer-bearing
   ``memcpy`` operations receive explicit constraints when the Aser frontend
   does not emit them. An unmapped LLVM pointer is conservatively reported as
   may-alias; it is never converted into a no-alias result.

``ValueFlowClient`` / ``lotus-cfl-vf``
   Implement SVF's second classical-CFL client, ``CFLVF``. The driver builds
   Lotus's AserPTA-backed SVFG and MemorySSA, removes dereference inputs and
   stale strong-update flow, keeps ``direct``, ``indirect``, and ``thread``
   terminals distinct, and encodes
   call/return edges as matched ``call_i``/``ret_i`` terminals, and solves
   context-sensitive value-flow reachability with any classical backend. The
   derived ``A`` relation is the sound union of those edge categories, not a
   path-feasibility or memory-object proof.

Alias and value-flow relation queries require ``solve()`` first and throw a
``logic_error`` when called on an unsolved client.

``ReachabilityStats`` separates session snapshots (graph/relation sizes and
payload estimates) from work performed by the current ``solve()`` call
(iterations, duplicates, peak worklist, timing, and transitive propagation).
``solveToFixedPoint`` sums per-call work and retains the final snapshots.


Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-classical lotus-cfl-alias lotus-cfl-vf
   build/bin/lotus-cfl-classical \
     --grammar grammar.txt --graph graph.txt --solver sparse-bitvector --json-stats

   build/bin/lotus-cfl-alias --solver sparse-bitvector --encoding pag \
     --check-annotations module.bc

   build/bin/lotus-cfl-vf --solver transitive-closure \
     --query main::source,main::sink module.bc

Use ``--graph-mode plain|matrix|pag-matrix`` and
``--direction plain|reverse|bidirectional`` to state input semantics.
Attributed domains are inferred from graph labels. ``--attribute-domain`` can
override a variable (``var:i=1,2``) or symbol kind (``kind:call=1,2``).
``--relation-output``, ``--stats-output``, ``--start-only``, and
``--validate-only`` support reproducible batch workflows. JSON statistics
include grammar/graph sizes, worklist behavior, estimated container payload,
timings, and transitive-closure propagation statistics. Payload estimates are
not RSS or allocator measurements and must not be used as real memory totals.

Input formats
-------------

Text graphs contain one ``source,target,label`` edge per line. The line-oriented
DOT subset accepts ``label=...`` edge attributes and quoted IDs; it is not a
general DOT language parser. JSON accepts ``nodes`` (strings or objects with
``id``/``name``) and ``edges`` containing ``source``, ``target``, and ``label``.
