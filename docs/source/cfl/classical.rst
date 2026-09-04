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

``Solvers/SolverSession``
   Public backend selection, incremental session state, and solver
   orchestration.

``Solvers/Engines/``
   Reusable relation engines. ``TransitiveClosure`` is the generic incremental
   closure engine. ``Engines/PEARL/``, ``Engines/POCR/``, ``Engines/SQID/``,
   and ``Engines/STG/`` contain the paper algorithms; ``Engines/POCR/`` also
   contains its specialized alias/value-flow engines and client grammars.

``Solvers/Preprocessing/``
   Graph simplification and RSM-guided foldability analysis.

``Solvers/ConstraintGrounding``
   The separate structural constraint-grounding analysis.

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

``Solvers/Engines/POCR/ClientGrammars``
   Holds POCR's exact standard and grammar-rewritten production tables for the
   alias and value-flow engines. These are data selections over the shared
   solver, not additional clients.

Solver backends
---------------

``SparseSet``
   Conventional indexed worklist saturation with hash-set relations.

``SparseBitVector``
   The same worklist algorithm with per-node, per-symbol LLVM sparse
   bitvectors. This is a storage choice, not the POCR algorithm.

``Graspan``
   Executes POCR's source-ordered epoch/delta evaluation. Each source combines
   its ``old`` and ``new`` facts in the same four phases as
   ``GspanAA``/``GspanVFA``, then immediately updates that source's two
   relations. Old sources are revisited while any middle-node delta remains.

``Pearl``
   Implements ASE 2023 multi-derivation with separate non-transitive,
   partially transitive, and fully transitive propagation. Select it with
   ``--solver pearl``.

``Sqid``
   Implements OOPSLA 2026 adaptive and differential relation chaining with
   dual old/delta graph views. Select it with ``--solver sqid``.

``TransitiveClosure``
   Uses sparse bitvectors generally and a dedicated incremental forward/reverse
   bitvector closure for every production ``X -> X X``. Inserting ``u -> v``
   crosses predecessors of ``u`` with successors of ``v``. It does not retain
   copied reachability trees or a second hash-set closure.

``Pocr``
   Ports POCR's paired predecessor-tree/successor-tree propagation to Lotus
   containers. Primary arcs and secondary closure facts follow the original
   FIFO scheduling, and linear-recursive rules use early-pruned tree traversal
   instead of generic relation joins. Sparse bitvectors expose the complete
   relation to clients and preserve non-empty-path reflexive pairs introduced
   by cycles.

``HierarchicalPocr``
   Uses the same paired-tree closure as ``Pocr`` and prioritizes facts for
   transitive symbols ahead of the ordinary grammar worklist. Select it with
   ``--solver hpocr``.

``FullyOrdered``
   Ports FOCR's forward/backward edge-critical-graph maintenance. The critical
   graph is a reduced reachability skeleton, while the public relation remains
   the complete exact CFL relation. Select it with ``--solver focr``; add
   ``--focr-scc`` for POCR's optional critical-graph cycle simplification.

All backends return exactly the same grammar-relative relation. Tests compare
their complete triples, not only start-symbol answers, against an independent
cubic recognizer and exercise incremental additions and non-nullable cycles.

``ConstraintGroundingSolver`` is separate: it computes structural set-variable
grounding statistics and does not expose a CFL node-pair relation.

POCR support utilities
----------------------

``GraphSimplification`` ports the client preprocessing passes without SVF:
direct-edge SCC elimination, PEG/IVFG folding, common-dereference merging, and
FastDyck-style pruning of non-contributing edges. The general driver exposes
these through ``--scc-elimination``, ``--graph-folding``,
``--interdyck-pruning``, and ``--simplification-flavor alias|value-flow``.
Direct foldable pairs are collected once after SCC elimination; alias
common-dereference merging and FastDyck then use their own worklists, preserving
the original phase boundaries.

``RecursiveStateMachine`` and ``FoldabilityChecker`` implement POCR's RSM
transition semantics and node-pair foldability proof. The
``lotus-cfl-foldability`` tool reads an RSM and a pattern file.

``lotus-cfl-pocr`` runs the four hand-specialized engines directly on POCR
``.peg``/``.vfg`` datasets and the standard, Graspan, grammar-rewritten, and
rewritten-Graspan client engines. This is an engine driver; it does not
introduce a third analysis client. It also exposes ``--scc``, ``--graph-folding``,
``--interdyck``, ``--simplify-graph``, ``--graph-output``, and ``--focr-scc``.

``SolverOptions::unidirectional`` implements POCR's ``Insert``/``Follow``
summarization discipline. All facts remain available as exact output, while
only terminals, nullable seeds, and ``Insert`` symbols are indexed as future
join candidates. Use ``--unidirectional`` in the general driver.

See :doc:`pocr_migration` for the complete source-to-Lotus mapping and the
algorithms intentionally merged with an existing implementation.
See :doc:`pearl`, :doc:`stg`, and :doc:`sqid` for the papers, key ideas,
published algorithms, Lotus adaptations, and validation boundaries.

Adapters
--------

Adapter implementations are split by dependency:

``CanaryClassicalCFLAliasClient``
   PAG and PEG encodings, ``AliasClient``, and ``solveToFixedPoint`` for
   alternating client-defined discovery with incremental saturation. PEG
   loads and stores added after solving are converted through existing or
   reusable synthetic dereference nodes.
   ``solveSpecialized(Pocr|Focr)`` selects the hand-specialized engine without
   creating a generic ``SolverSession``.

``CanaryClassicalCFLValueFlowClient``
   SVFG preparation, value-flow encoding, ``ValueFlowClient``, and its
   grammar-driven and specialized engine integration.

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
   ``solveSpecialized(Pocr|Focr)`` selects the native vertical-propagation
   engine.

Alias and value-flow relation queries require ``solve()`` first and throw a
``logic_error`` when called on an unsolved client.

``ReachabilityStats`` separates session snapshots (graph/relation sizes and
payload estimates) from work performed by the current ``solve()`` call
(iterations, duplicates, peak worklist, timing, and transitive propagation).
``solveToFixedPoint`` sums per-call work and retains the final snapshots.


Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-classical lotus-cfl-alias \
     lotus-cfl-vf lotus-cfl-foldability lotus-cfl-pocr
   build/bin/lotus-cfl-classical \
     --grammar grammar.txt --graph graph.txt --solver sparse-bitvector --json-stats

   build/bin/lotus-cfl-alias --solver sparse-bitvector --encoding pag \
     --check-annotations module.bc

   build/bin/lotus-cfl-alias --engine pocr-aa --encoding peg \
     --check-annotations module.bc

   build/bin/lotus-cfl-vf --solver transitive-closure \
     --query main::source,main::sink module.bc

   build/bin/lotus-cfl-vf --engine focr-vfa \
     --query main::source,main::sink module.bc

   build/bin/lotus-cfl-pocr --engine pocr-aa --graph input.peg \
     --query 10,20 --json-stats

   build/bin/lotus-cfl-pocr --engine grgspan-aa --graph input.peg \
     --json-stats

   build/bin/lotus-cfl-pocr --engine focr-vfa --graph input.vfg \
     --simplify-graph --focr-scc --graph-output reduced.vfg

   build/bin/lotus-cfl-classical \
     --grammar grammar.txt --graph graph.txt --solver pocr --json-stats

   build/bin/lotus-cfl-classical \
     --grammar pocr.cfg --graph input.peg --solver graspan \
     --unidirectional --simplification-flavor alias --simplify-graph

Use ``--graph-mode plain|matrix|pag-matrix`` and
``--direction plain|reverse|bidirectional`` to state input semantics.
Attributed domains are inferred from graph labels. ``--attribute-domain`` can
override a variable (``var:i=1,2``) or symbol kind (``kind:call=1,2``).
``--relation-output``, ``--graph-output``, ``--stats-output``,
``--start-only``, and ``--validate-only`` support reproducible batch workflows.
JSON statistics
include grammar/graph sizes, worklist behavior, estimated container payload,
timings, transitive-closure propagation, POCR tree, and FOCR critical-graph
statistics. Payload estimates are not RSS or allocator measurements and must
not be used as real memory totals.

Input formats
-------------

Text graphs contain one ``source,target,label`` edge per line. POCR/SVF-style
tabular ``source target label [attribute]`` files (including ``.peg`` and
``.vfg``) are accepted directly; ``call_i 7`` is normalized to ``call_7``.
POCR's singular ``Production:`` grammar syntax and ``Insert:``, ``Follow:``,
and ``Count:`` sections are parsed natively. In this legacy syntax an indexed
LHS derived from non-indexed symbols receives index zero, and an ``_i`` graph
edge without an explicit fourth field is likewise normalized to index zero,
matching POCR. The line-oriented DOT subset
accepts quoted IDs and ``label=...`` edge attributes; JSON accepts ``nodes`` or
``vertices`` plus labeled ``edges``.
