# Control-dependence analysis

This library provides Lotus-native LLVM basic-block adapters for baseline
control-dependence algorithms migrated from
[dg](https://github.com/mchalupa/dg) and newer compact inevitability/biclique
algorithms. Include
`Analysis/ControlDependence/ControlDependence.h` and construct
`lotus::cd::ControlDependenceAnalysis` for a function.

Whole-ICFG clients include
`Analysis/ControlDependence/ICFGControlDependence.h` and link the separate
`CanaryICFGControlDependence` adapter target.

Reusable declarations live under `include/Analysis/ControlDependence/`.
Baseline implementations are split into `SCD.cpp`, `NTSCD.cpp`, `DOD.cpp`,
and `ControlClosure.cpp`. The new compact algorithms live separately in
`CompactNTSCD.cpp`, `CompactDOD.cpp`, and `CompactClosure.cpp`, preserving the
old implementations as experimental baselines. `ControlDependence.cpp` and
`ICFGControlDependence.cpp` are LLVM/Lotus graph adapters.

The core algorithms and function adapter are linked as
`CanaryControlDependence`. The optional whole-ICFG adapter is isolated in
`CanaryICFGControlDependence`, so function-level users such as the PDG do not
acquire an unnecessary ICFG dependency.

Supported algorithms:

| `Algorithm` | Meaning |
|---|---|
| `Standard` / `SCD` | Ferrante–Ottenstein–Warren standard control dependence |
| `NTSCD` | Non-termination-sensitive control dependence |
| `NTSCD2` | Backwards-counter NTSCD implementation |
| `NTSCDLegacy` | Compatibility name for dg's legacy backwards-counter implementation |
| `NTSCDRanganath` | Fixed-point form of Ranganath et al.'s NTSCD algorithm |
| `NTSCDRanganathOriginal` | Original order-sensitive algorithm, retained for comparison |
| `DOD` | Decisive-order dependence |
| `DODRanganath` | Ranganath et al.'s DOD algorithm |
| `DODNTSCD` | Combined DOD and NTSCD relation |
| `StrongControlClosure` | Experimental strong control closure |
| `NTSCDCompact` | All-target inevitability matrix plus multiway NTSCD |
| `DODCompact` | SCC-based canonical DOD bicliques |
| `DODNTSCDCompact` | Shared inevitability, compact DOD, and incidence closure |

`getDependencies(block)` returns the predicate blocks on which `block`
depends. `getDependents(predicate)` returns the inverse relation. Results use
LLVM function order. Strong closure is queried with `getClosure()` and has no
binary dependence relation.

Compact DOD additionally exposes `hasDODBiclique`, `getDODLeft`,
`getDODRight`, and exact pair membership through `isDOD`. These queries keep
the canonical complete-bipartite representation instead of enumerating its
Cartesian product. `getDependencyClosure` computes the least seed superset
closed under compact NTSCD and DOD using reverse incidences and two side-hit
bits per decision.

For a graph with `n` vertices and `m` edges, compact preprocessing takes
`O(n(n+m))` time. On binary CFGs this is `O(n^2)`. Exact enumeration of `K`
DOD triples takes `O(n(n+m)+K)`, while membership does not enumerate pairs.

As in dg, DOD is represented as a binary over-approximation of its underlying
ternary relation. The migrated DOD implementation accepts binary predicates;
multi-way switches are skipped. The original Ranganath NTSCD variant is known
to be incorrect and is exposed only for parity and experimentation.

The function API is intraprocedural and block-granular.
`ICFGControlDependenceAnalysis` runs every graph-based variant over an existing
Lotus ICFG, corresponding to dg's whole-ICFG mode. Standard CD remains
function-only because it requires a function post-dominator tree. The migration
does not include dg's separate no-return call analysis; Lotus's ICFG directly
models calls, returns, exceptional returns, and non-returning calls. Fully
resolved call-to-return summary edges are excluded from whole-ICFG analysis;
summary edges are retained for unresolved or external callees.

## dg compatibility notes

The migrated algorithms retain dg's relation orientation and core steps:

- SCD uses dg's recursive post-dominance-frontier construction.
- NTSCD, NTSCD2, and original/fixed Ranganath propagation follow dg's
  initialization, worklist, and fixed-point rules.
- DOD uses the same all-maximal-path sets, `A_p` projection, blue/red coloring,
  and binary projection of the ternary relation.
- DOD+NTSCD and strong control closure retain dg's combined and
  theta/gamma-based formulations.

The following intentional correctness and robustness fixes differ from dg:

- counters use `size_t` rather than 16-bit values, and already-colored nodes
  are not re-enqueued, preventing counter underflow on self-loops or closure
  target cycles;
- Ranganath DOD records both dependent endpoints in the reverse map; dg's
  `revCD[n].insert(n)` records the predicate itself and violates the
  forward/reverse invariant;
- DOD skips predicates with other than two distinct successors instead of
  asserting on LLVM multi-way switches;
- public results are emitted in stable LLVM function/ICFG node order.
