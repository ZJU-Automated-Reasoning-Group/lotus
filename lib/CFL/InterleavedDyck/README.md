# Interleaved-Dyck Reachability

This directory is the umbrella for Lotus's interleaved-Dyck implementations.
The same structure is mirrored by `include/CFL/InterleavedDyck`.

| Subdirectory | Role | Guarantee or result |
|---|---|---|
| [`Core`](Core/README.md) | Typed labels, graphs, DOT parsing, unary projection, and shared bidirected-Dyck support | Representation and common algorithms |
| [`Unary`](Unary/README.md) | Adaptive and fixed-counter algorithms for bidirected unary `D1`-interleaved-`D1` | Exact component partition after unary projection |
| [`StagedBounds`](StagedBounds/README.md) | Projected languages, union-Dyck lower bounds, parity refinement, and on-demand checks | Certified lower bound and progressively tighter upper bounds |
| [`MCFL`](MCFL/README.md) | Normal-form MCFG solver and the dimension-indexed `G_d` hierarchy | Exact for a supplied MCFG; underapproximation for generated Interleaved-Dyck grammars |
| [`MutualRefinement`](MutualRefinement/README.md) | Integer CNF saturation and derivation tracing used by staged bounds | Grammar-relative reachability and contributing-edge closure |
| [`GraphReduction`](GraphReduction/README.md) | PLDI 2020 file-oriented graph simplification | Reduced graph preserving the reduction's reachability property |

## Public namespaces

```text
lotus::cfl::interleaved_dyck
├── unary
├── staged_bounds
├── mcfl
└── mutual_refinement
```

`Core` owns the types directly in `lotus::cfl::interleaved_dyck`; the other
public modules use nested namespaces matching their directories.

## Choosing an analysis

| Question | API |
|---|---|
| Exact bidirected unary reachability | `interleaved_dyck::unary::AdaptiveSolver` |
| POPL 2022 fixed-counter baseline | `interleaved_dyck::unary::FixedCounterSolver` |
| Typed lower and upper bounds | `interleaved_dyck::staged_bounds::Solver` |
| Dimension-indexed certified pairs | `interleaved_dyck::mcfl::InterleavedDyckSolver` |
| Exact reachability for a client-supplied MCFG | `interleaved_dyck::mcfl::Solver` |

`Core`, `StagedBounds`, and `MCFL` preserve the directed arcs supplied by the
input. `Unary` rejects non-bidirected input by default; explicit
symmetrization changes its interpretation to an overapproximation of the
original directed graph. `GraphReduction` uses private synthetic orientations
inside its reduction and does not expose them as original input arcs.
