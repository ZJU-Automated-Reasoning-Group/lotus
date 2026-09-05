Control-Dependence Analysis
===========================

Control-dependence analysis over LLVM basic blocks and Lotus ICFGs.

**Headers**: ``include/Analysis/ControlDependence``

**Implementation**: ``lib/Analysis/ControlDependence``

**Main components**:

- ``ControlDependenceAnalysis`` – block-level adapter for one LLVM function
- ``ICFGControlDependenceAnalysis`` – whole-ICFG adapter over a Lotus ICFG
- ``Algorithm`` – selects the control-dependence variant to compute
- ``Graph`` / ``GraphNode`` – generic graph shared by the algorithms and adapters

The library provides Lotus-native LLVM basic-block adapters for baseline
control-dependence algorithms migrated from
`dg <https://github.com/mchalupa/dg>`_ and newer compact
inevitability/biclique algorithms. Baseline implementations are split into
``SCD.cpp``, ``NTSCD.cpp``, ``DOD.cpp``, and ``ControlClosure.cpp``. The
compact algorithms live separately in ``CompactNTSCD.cpp``, ``CompactDOD.cpp``,
and ``CompactClosure.cpp``, preserving the old implementations as experimental
baselines. ``ControlDependence.cpp`` and ``ICFGControlDependence.cpp`` are the
LLVM/Lotus graph adapters.

The core algorithms and function adapter are linked as
``CanaryControlDependence``. The optional whole-ICFG adapter is isolated in
``CanaryICFGControlDependence``, so function-level users such as the PDG do not
acquire an unnecessary ICFG dependency.

Supported algorithms
--------------------

The ``Algorithm`` enum selects the variant to compute:

- ``Standard`` / ``SCD`` – Ferrante-Ottenstein-Warren standard control dependence
- ``NTSCD`` – non-termination-sensitive control dependence
- ``NTSCD2`` – backwards-counter NTSCD implementation
- ``NTSCDLegacy`` – compatibility name for dg's legacy backwards-counter implementation
- ``NTSCDRanganath`` – fixed-point form of Ranganath et al.'s NTSCD algorithm
- ``NTSCDRanganathOriginal`` – original order-sensitive algorithm, retained for comparison
- ``DOD`` – decisive-order dependence
- ``DODRanganath`` – Ranganath et al.'s DOD algorithm
- ``DODNTSCD`` – combined DOD and NTSCD relation
- ``StrongControlClosure`` – experimental strong control closure
- ``NTSCDCompact`` – all-target inevitability matrix plus multiway NTSCD
- ``DODCompact`` – SCC-based canonical DOD bicliques
- ``DODNTSCDCompact`` – shared inevitability, compact DOD, and incidence closure

The function API is intraprocedural and block-granular.
``getDependencies(block)`` returns the predicate blocks on which ``block``
depends; ``getDependents(predicate)`` returns the inverse relation. Results use
LLVM function order. Strong closure is queried with ``getClosure()`` and has no
binary dependence relation.

Compact DOD additionally exposes ``hasDODBiclique``, ``getDODLeft``,
``getDODRight``, and exact pair membership through ``isDOD``. These queries
keep the canonical complete-bipartite representation instead of enumerating its
Cartesian product. ``getDependencyClosure`` computes the least seed superset
closed under compact NTSCD and DOD using reverse incidences and two side-hit
bits per decision.

Whole-ICFG analysis
-------------------

``ICFGControlDependenceAnalysis`` runs every graph-based variant over an
existing Lotus ICFG, corresponding to dg's whole-ICFG mode. Standard CD remains
function-only because it requires a function post-dominator tree. The ICFG
directly models calls, returns, exceptional returns, and non-returning calls.
Fully resolved call-to-return summary edges are excluded from whole-ICFG
analysis; summary edges are retained for unresolved or external callees.

Basic usage (C++)
-----------------

.. code-block:: cpp

   #include <Analysis/ControlDependence/ControlDependence.h>

   llvm::Function &F = ...;
   lotus::cd::ControlDependenceOptions options;
   options.algorithm = lotus::cd::Algorithm::NTSCD;
   lotus::cd::ControlDependenceAnalysis cd(F, options);

   llvm::BasicBlock *B = ...;
   for (const llvm::BasicBlock *predicate : cd.getDependencies(B))
     // B is control dependent on predicate's terminator.

Interpretation
--------------

DOD is represented as a binary over-approximation of its underlying ternary
relation, as in dg. The migrated DOD implementation accepts binary predicates;
multi-way switches are skipped. The original Ranganath NTSCD variant is known
to be incorrect and is exposed only for parity and experimentation. For a graph
with ``n`` vertices and ``m`` edges, compact preprocessing takes ``O(n(n+m))``
time; on binary CFGs this is ``O(n^2)``. Exact enumeration of ``K`` DOD triples
takes ``O(n(n+m)+K)``, while membership does not enumerate pairs.

See also :doc:`cfg` and :doc:`../tools/ir/index`.