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

Interleaved-Dyck Core
---------------------

Shared typed ``Label``, ``Edge``, ``Graph``, and ``Pair`` types plus the DOT
parser used by interleaved-Dyck benchmark datasets.

**Location**: ``include/CFL/InterleavedDyckCore/``,
``lib/CFL/InterleavedDyckCore/``

Approximation consumes this graph directly, UnaryInterleavedDyck applies unary
projection, and MCFL converts it through a typed-to-generic adapter.

Exact Unary Interleaved Dyck
----------------------------

Exact component reachability for bidirected unary
``D1``-interleaved-``D1``. The module provides adaptive counter flattening and
the POPL 2022 fixed-counter exact baseline.

**Location**: ``include/CFL/UnaryInterleavedDyck/``,
``lib/CFL/UnaryInterleavedDyck/``

See :doc:`unary_interleaved_dyck` for both algorithms, their exactness boundary,
and benchmark eligibility rules.

Interleaved-Dyck Approximation
------------------------------

Staged under- and overapproximation for reachability under two interleaved
families of Dyck constraints.

**Location**: ``include/CFL/InterleavedDyckApproximation/``,
``lib/CFL/InterleavedDyckApproximation/``

This component computes a certified union-Dyck lower bound and progressively
tighter projected-CFL upper bounds; it is not an exact solver for the general
typed problem.

**Features**:

* DOT parsing for parenthesis, bracket, and neutral edges
* Dyck-over-the-union underapproximation
* Projected-language intersection and derivation-tracing mutual refinement
* Stronger parity grammar and pairwise on-demand refinement
* Taint and value-flow benchmark modes

Multiple Context-Free Language Reachability
-------------------------------------------

All-pairs reachability for non-deleting, non-permuting MCFGs and the POPL 2025
typed underapproximation hierarchy.

**Location**: ``include/CFL/MCFL/``, ``lib/CFL/MCFL/``

**Features**:

* All five normal-form MCFL rule types with structural validation
* Indexed worklist saturation and tuple reachability pruning
* Concrete path witnesses from retained derivation DAGs
* ``G_d^circ`` and ``G_d^+`` grammar generation for arbitrary dimensions
* Artifact-compatible staged condensation, DOT input, and command-line tool
* Adapter from the shared typed interleaved-Dyck graph

Guarantee Summary
-----------------

.. list-table:: Choosing an interleaved-Dyck implementation
   :header-rows: 1
   :widths: 31 39 30

   * - API
     - Intended use
     - Guarantee
   * - ``mcfl::InterleavedDyckSolver``
     - Certified typed pairs through ``G_d``
     - Underapproximation
   * - ``unary_interleaved_dyck::FixedCounterSolver``
     - POPL 2022 exact fixed-counter baseline
     - Exact component partition
   * - ``unary_interleaved_dyck::AdaptiveSolver``
     - Bidirected unary projection
     - Exact component partition
   * - ``interleaved_dyck_approximation::Solver``
     - Typed lower/upper refinement
     - Approximation bounds

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

PLDI 2020 interleaved-Dyck graph simplification. This component transforms a
DOT graph and does not itself return the final reachability relation.

**Location**: ``lib/CFL/InterDyckGraphReduce/``

**Features**:

* Two-color summary construction and degree-based node merging
* Iterative Python orchestration until no further edge is removed
* Explicit directed versus already-bidirected input mode
* Private legacy summary representation under the ``lib`` subtree

MutualRefinement
----------------

Grammar-agnostic CNF reachability and derivation tracing used by refinement
experiments and by ``InterleavedDyckApproximation``.

**Location**: ``lib/CFL/MutualRefinement/``

**Features**:

* Integer-encoded ``CnfGrammar`` and ``CnfGraph`` representation
* CFL saturation with unary and binary derivation records
* Backward closure to contributing input edges
* Generic file-driven alternating-refinement experiment

It does not own typed delimiter semantics, approximation grammars, benchmark
preprocessing, or lower/upper-bound interpretation; those belong to
``InterleavedDyckApproximation``.

See also:

- :doc:`classical`
- :doc:`csindex`
- :doc:`unary_interleaved_dyck`
- :doc:`interleaved_dyck_approximation`
- :doc:`inter_dyck_graph_reduce`
- :doc:`mcfl`
- :doc:`mutual_refinement`
