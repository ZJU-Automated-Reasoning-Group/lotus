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
   instantiates correlated attributed symbols such as ``call_i``/``ret_i``.

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
   Uses the POCR relation and recognizes every production ``X -> X X`` as a
   transitive relation. It handles those rules with a specialized dynamic
   transitive-closure step rather than depending on a hard-coded symbol name.

All backends return exactly the same grammar-relative relation. Tests compare
their complete triples, not only start-symbol answers.

Adapters
--------

The core target ``CanaryClassicalCFL`` has no SVFG or pointer-analysis
dependency. Adapter implementations are split by dependency:

``CanaryClassicalCFLAlias``
   PAG and PEG encodings, ``AliasClient``, and ``solveToFixedPoint`` for
   alternating client-defined discovery with incremental saturation.

``AserConstraintAdapter.h``
   A header-only converter from Lotus's native AserPTA constraint graph to the
   alias client input. It is separate from the generic core.

``CanaryClassicalCFLSVFG``
   ``SVFGAdapter`` and ``ValueFlowClient`` for call/return-matched value flow.

Clients include ``Alias.h``, ``AserConstraintAdapter.h``, or
``SVFGAdapter.h`` directly; there is no compatibility umbrella.

Command line
------------

.. code-block:: console

   cmake --build build --target lotus-cfl-classical
   build/bin/lotus-cfl-classical \
     --grammar grammar.txt --graph graph.txt --solver pocr --json-stats

Use ``--graph-mode plain|matrix|pag-matrix`` and
``--direction plain|reverse|bidirectional`` to state input semantics.
``--attributes 1,2,3`` supplies the observed domain for attributed grammar
variables. ``--dump-relation`` emits ``source,target,symbol`` triples.

Input formats
-------------

Text graphs contain one ``source,target,label`` edge per line. Plain DOT uses
``label=...`` edge attributes. JSON accepts an object containing edge objects
with ``source``, ``target``, and ``label`` fields.

See :doc:`svf_migration` for provenance and the capability migration record.
