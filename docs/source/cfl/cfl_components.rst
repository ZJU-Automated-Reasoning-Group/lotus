CFL Reachability Components
===========================

Advanced CFL reachability algorithms and graph analysis frameworks.

Classical CFL Reachability
--------------------------

Grammar-driven CFL reachability utilities, solver backends, and SVF adapters.

**Location**: ``include/CFL/Classical/``, ``lib/CFL/Classical/``

**Features**:
* Grammar parsing and CNF/STBDU normalization helpers
* Labeled graph construction for text, DOT, PAG, and PEG-style encodings
* Classical and set-constraint solvers for reachability closure
* Adapters for alias and value-flow problems built on SVF structures

Interleaved-Dyck Approximation
------------------------------

Staged under- and overapproximation for reachability under two interleaved
families of Dyck constraints.

**Location**: ``include/CFL/InterleavedDyck/``,
``lib/CFL/InterleavedDyck/``

**Features**:

* DOT parsing for parenthesis, bracket, and neutral edges
* Dyck-over-the-union underapproximation
* Projected-language intersection and derivation-tracing mutual refinement
* Stronger parity grammar and pairwise on-demand refinement
* Taint and value-flow benchmark modes

Multiple Context-Free Language Reachability
-------------------------------------------

All-pairs reachability for non-deleting, non-permuting MCFGs, plus the POPL
2025 MCFL underapproximation hierarchy for interleaved Dyck languages.

**Location**: ``include/CFL/MCFL/``, ``lib/CFL/MCFL/``

**Features**:

* All five normal-form MCFL rule types with structural validation
* Indexed worklist saturation and tuple reachability pruning
* Concrete path witnesses from retained derivation DAGs
* ``G_d^circ`` and ``G_d^+`` grammar generation for arbitrary dimensions
* Artifact-compatible staged condensation, DOT input, and command-line tool

CSIndex (Context-Sensitive Indexing)
------------------------------------

Context-sensitive indexing for CFL reachability.

**Location**: ``lib/CFL/CSIndex/``

**Features**: Context-aware indexing algorithms for efficient CFL queries.

**Components**:
* Context-sensitive graph indexing
* Reachability query optimization
* Memory-efficient representations


InterDyckGraphReduce
--------------------

Interprocedural Dyck graph reduction algorithms.

**Location**: ``lib/CFL/InterDyckGraphReduce/``

**Features**: Interprocedural analysis with graph reduction techniques for Dyck languages.

MutualRefinement
----------------

Mutual refinement algorithms for CFL analysis.

**Location**: ``lib/CFL/MutualRefinement/``

**Features**: Bidirectional refinement techniques for improving analysis precision.

See also:

- :doc:`classical`
- :doc:`csindex`
- :doc:`interleaved_dyck_approximation`
- :doc:`inter_dyck_graph_reduce`
- :doc:`mcfl`
- :doc:`mutual_refinement`
