POCR Migration Matrix
=====================

The POCR implementation is represented in Lotus without an SVF dependency.
This page records every algorithm-bearing POCR source group and whether Lotus
ports it directly or merges it into an equivalent existing component.

General solvers
---------------

``StdCFL``
   ``SolverBackend::SparseSet`` and ``SparseBitVector`` implement the indexed
   classical worklist algorithm.

``PocrCFL``
   ``SolverBackend::Pocr`` uses paired predecessor and successor reachability
   trees. Primary transitive arcs are registered before tree traversal;
   closure pairs are then queued as secondary facts. The ``X -> X A`` and
   ``X -> A X`` cases traverse the appropriate tree and prune a subtree when
   its summary edge already exists, as in ``checkStree``/``checkPtree``.
   Structural identity is kept separate from semantic epsilon, so non-nullable
   cycles correctly derive reflexive pairs.

``HPocrCFL``
   ``SolverBackend::HierarchicalPocr`` adds POCR's separate FIFO primary list,
   drains it before the ordinary FIFO worklist, and repeats when ordinary
   processing produces new primary facts.

``FocrCFL`` and ``TRFocrCFL``
   Merged as ``SolverBackend::FullyOrdered``. Its edge-critical graph is the
   reduced storage, while its public relation exposes the exact closure. The
   TR variant's separate secondary container is subsumed by this relation
   abstraction rather than exposed as a second selector; ECG insertion,
   redundant-critical-edge removal, and primary/secondary scheduling remain.

``TRCFL``
   Merged into ``SolverBackend::TransitiveClosure``. Lotus's per-symbol
   incremental closure already separates transitive pairs from the ordinary
   relation and omits the literal ``X -> X X`` worklist join. POCR's version has
   unresolved secondary-edge TODOs, so it is not retained as a duplicate mode.

``GspanAA`` and ``GspanVFA``
   Generalized as ``SolverBackend::Graspan`` while retaining POCR's two
   relations and source-ordered epoch update. For every source it evaluates
   ``old + new``, unary ``new``, ``new + old``, and ``new + new``, then moves
   that source's delta into ``old`` and installs its next delta. Sources with
   no local delta are still revisited for ``old + new`` joins through a middle
   node, matching the artifact implementation.

``GRAA``, ``GRVFA``, ``GRGspanAA``, and ``GRGspanVFA``
   These classes differ only by hard-coded production tables.
   ``buildPocrClientGrammar`` owns exact standard and rewritten alias/value-flow
   tables; ``gr-aa``/``gr-vfa`` pair them with the worklist and
   ``grgspan-aa``/``grgspan-vfa`` pair them with Graspan. The larger artifact
   ``aanew.cfg``/``vfnew.cfg`` grammars also load through the canonical
   ``Grammar`` frontend. Both forms have regression coverage. No duplicate
   solver class is needed.

Clients and preprocessing
-------------------------

``StdAA``/``PocrAA``/``FocrAA``
   ``StdAA`` remains the grammar-driven path. ``PocrAA`` and ``FocrAA`` are
   native ``PocrAliasEngine`` and ``FocrAliasEngine`` implementations under
   ``Solvers/Engines/POCR``. They preserve POCR's horizontal propagation,
   symmetric ``V``/``M`` facts, dereference matching, attributed field
   matching, and one-sided hybrid-tree or ECG traversal. ``AliasClient`` is
   the sole alias client and selects these engines through
   ``solveSpecialized``. Optional ECG cycle simplification is available for
   ``FocrAliasEngine``.

``StdVFA``/``PocrVFA``/``FocrVFA``
   ``StdVFA`` remains the grammar-driven path. ``PocrVFA`` and ``FocrVFA`` are
   native ``PocrValueFlowEngine`` and ``FocrValueFlowEngine`` implementations
   under ``Solvers/Engines/POCR``. They preserve online reachability insertion
   and vertical ``call_i A ret_i`` matching. ``ValueFlowClient`` is the sole
   value-flow client and selects these engines through
   ``solveSpecialized``. Optional ECG cycle simplification is available for
   ``FocrValueFlowEngine``.

``SCCElimination``, ``PEGFold``, and ``IVFGFold``
   Ported by ``GraphSimplification`` using Lotus's non-recursive Tarjan utility,
   representative maps, source-node preservation, and client-specific folding.
   Foldable direct pairs are detected once on the post-SCC graph and merged
   before the separate dynamic common-dereference phase, matching POCR's phase
   order rather than repeatedly redetecting direct pairs.
   POCR ``d`` is mapped to the physical Lotus PEG ``addrbar`` orientation;
   ``vgep`` and Lotus indirect/thread flow edges participate in their
   corresponding direct-edge reductions.

``PEGInterDyck`` and ``IVFGInterDyck``
   Ported as the ``prune_interdyck`` graph-simplification phase. Lotus's
   separate ``InterDyckGraphReduce`` remains the single implementation of the
   stronger PLDI'20 reduction pipeline; it is not copied into ``Classical``.

``RSM`` and ``GFPattern``
   Ported as ``RecursiveStateMachine``, ``NodePairPattern``, and
   ``FoldabilityChecker``, with the ``lotus-cfl-foldability`` driver. The port
   implements the intended false-state and two-box enumeration guards instead
   of preserving the inverted/accumulating conditions in the artifact source.

Formats, relations, and controls
--------------------------------

``CFG`` and ``CFLGraph``/``PEG``/``IVFG``
   Merged into ``Grammar`` and ``LabeledGraph``. Both modern Lotus formats and
   POCR's tabular attributed formats are accepted directly. Standard and
   rewritten alias engines complete ``abar``/``dbar``/``fbar_i`` exactly as
   POCR's PEG initializer does, so physical-only and already-bidirectional PEG
   files have the same result. Legacy attributed productions retain POCR's
   runtime rule that an indexed head gets index zero when neither RHS symbol is
   indexed; omitted graph-edge indices also default to zero.

``CFLData`` and ``HybridData``
   Replaced by Lotus ``Relation`` backends and ``PocrTransitiveClosure``.

``ECG`` and ``BSECG``
   Merged into ``FullyOrderedTransitiveClosure``. The pointer and bitset ECG
   variants do not warrant separate public algorithms. POCR's optional
   ``ecgscc`` path is controlled by ``SolverOptions::simplify_focr_cycles`` or
   ``--focr-scc`` and is off by default, matching POCR. The same option applies
   to both specialized FOCR engines.

``CFLOpt::ucfl``
   Ported through ``SolverOptions::unidirectional`` and the grammar's
   ``Insert``/``Follow``/``Count`` metadata.

``CFLStat``, ``AAStat``, ``VFAStat``, and output options
   Merged into ``ReachabilityStats`` and the existing structured command-line
   reporting. ``--relation-output``, ``--graph-output``, ``--start-only``, and
   ``--json-stats`` cover relation pairs, normalized/preprocessed graphs, count
   symbols, and reproducible algorithm counters without process-global SVF
   options.

Statistics and graph/relation output use Lotus's existing structured CLI
reporting. Tests compare all exact solver relations with an independent cubic
recognizer over 1,000 generated problems, cover incremental updates, ECG cycle
simplification, primary/secondary scheduling, and tree-join execution, and run
POCR-format and rewritten client grammars through their applicable backends.
