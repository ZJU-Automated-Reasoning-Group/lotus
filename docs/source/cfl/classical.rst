Classical Grammar-Driven CFL Reachability
==========================================

``CFL/Classical`` is the general grammar-driven part of Lotus's CFL
subsystem. It is independent of the specialized interleaved-Dyck and MCFL
solver representations.

Architecture
------------

``Grammar``
   Parses declared start, terminal, and nonterminal symbols; normalizes
   whitespace-delimited ``*`` and ``?`` EBNF; binarizes long productions; and
   compiles string symbols to stable integer IDs. ``GrammarParseOptions``
   instantiates correlated attributed symbols such as ``call_i``/``ret_i``
   using variable-specific or per-symbol domains. Graph labels provide domains
   automatically when the command-line driver is used.

``LabeledGraph``
   Stores the base problem boundary. Solver sessions keep derived facts in a
   separate relation; only explicit incremental terminal additions modify the
   graph. Text, DOT, and JSON readers are available. Plain, reverse, and
   bidirectional transformations are explicit; the parser never silently
   applies them.

``Relation``
   Separates terminal and derived facts from the graph frontend. Sparse-set
   and LLVM sparse-bitvector implementations provide indexed successor and
   predecessor lookup.

``SolverSession``
   Retains a relation and worklist across calls. ``addTerminalEdge`` followed
   by ``solve`` supports clients that discover constraints incrementally.

Solver backends
---------------

``Baseline``
   Conventional indexed worklist saturation. This is the semantic reference
   backend.

``POCR``
   Uses per-node, per-symbol sparse predecessor and successor bitvectors.

``Hybrid``
   Uses the POCR relation plus a per-symbol reachability forest for every
   production ``X -> X X``. Every root owns a tree containing each reachable
   node at most once; inserting ``u -> v`` melds ``v``'s tree into all trees
   containing ``u``. Cycles, dynamic nodes, and multiple transitive symbols
   are supported without a hard-coded nonterminal name.
   Forests are authoritative storage for transitive facts; the bitvector
   relation stores only non-transitive symbols, while a composite relation view
   supplies uniform joins and queries without duplicating transitive closure.

All backends return exactly the same grammar-relative relation. Tests compare
their complete triples, not only start-symbol answers.

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
   does not emit them.


Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-classical
   build/bin/lotus-cfl-classical \
     --grammar grammar.txt --graph graph.txt --solver pocr --json-stats

   build/bin/lotus-cfl-alias --solver pocr --encoding pag \
     --check-annotations module.bc

Use ``--graph-mode plain|matrix|pag-matrix`` and
``--direction plain|reverse|bidirectional`` to state input semantics.
Attributed domains are inferred from graph labels. ``--attribute-domain`` can
override a variable (``var:i=1,2``) or symbol kind (``kind:call=1,2``).
``--relation-output``, ``--stats-output``, ``--start-only``, and
``--validate-only`` support reproducible batch workflows. JSON statistics
include grammar/graph sizes, worklist behavior, approximate relation memory,
timings, and hybrid-forest structure.

Input formats
-------------

Text graphs contain one ``source,target,label`` edge per line. Plain DOT uses
``label=...`` edge attributes. JSON accepts an object containing edge objects
with ``source``, ``target``, and ``label`` fields.
