SVF CFL Capability Migration Record
===================================

Purpose and provenance
----------------------

The local SVF distribution is AGPLv3 while Lotus is primarily MIT licensed.
SVF implementation files must therefore not be copied into the MIT core
without an explicit licensing decision. The Classical architecture is a
Lotus-native implementation of standard CFL algorithms and published ideas.
This record tracks behavioral compatibility; it is not permission to copy SVF
source. Lotus exposes only its native alias and SVFG adapter APIs; no SVF
compatibility API or wrapper is retained.

Capability matrix
-----------------

.. list-table:: Migration status
   :header-rows: 1
   :widths: 25 18 57

   * - SVF capability
     - Status
     - Lotus implementation
   * - GrammarBuilder / CFGrammar
     - Reimplemented
     - Typed symbol table, declared start symbol, terminal/nonterminal sets
   * - CFGNormalizer
     - Reimplemented
     - EBNF normalization, binarization, correlated attribute instantiation
   * - CFLGraph / checker
     - Reimplemented
     - LabeledGraph, explicit direction transforms, validation diagnostics
   * - Text / DOT / JSON builders
     - Reimplemented
     - GraphLoadOptions and the generic range-based graph builder
   * - Classical solver
     - Reimplemented
     - Baseline SolverSession backend
   * - POCR solver
     - Reimplemented
     - Sparse predecessor/successor bitvector relation
   * - POCR hybrid
     - Reimplemented differently
     - Generic transitive-rule specialization for every ``X -> X X``
   * - CFLAlias PAG / PEG
     - Integrated
     - AliasClient and native AserPTA constraint-graph adapter
   * - On-the-fly constraint discovery
     - Integrated hook
     - Incremental AliasClient/SolverSession; call-graph policy stays in AA
   * - CFLVF / SVFG construction
     - Adapted
     - Consumes Lotus SVFG rather than importing a second SVFG builder
   * - Statistics and driver
     - Reimplemented
     - ReachabilityStats and ``lotus-cfl-classical``
   * - CFLBase inheritance hierarchy
     - Intentionally rejected
     - Composition of grammar, graph, relation, solver, and client
   * - SVF global option plumbing
     - Intentionally rejected
     - Explicit C++ options and command-line arguments

Verification gates
------------------

* ``ClassicalCFLTest`` preserves the original grammar, graph, CNF, and SC
  behavior.
* ``ClassicalArchitectureTest`` compares complete baseline, POCR, and hybrid
  closures and covers incremental solving, attributes, JSON, and direction
  policies.
* ``ClassicalAdaptersTest`` covers native AserPTA conversion, PAG, PEG, and
  call/return-matched SVFG reachability.
* The command-line smoke test in ``benchmarks/cfl-classical`` exercises all
  three backends over the same fixture.

Semantic boundary
-----------------

This work does not merge Classical grammar IR with ``CSIndex``, MCFL,
MutualRefinement, or the interleaved-Dyck engines. Those components expose
different guarantees and retain their specialized representations. A future
facade may share query and statistics vocabulary without pretending their
languages or approximation guarantees are identical.
