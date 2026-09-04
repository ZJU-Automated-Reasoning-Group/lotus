# Interleaved-Dyck reachability dataset

The `taint/` and `valueflow/` directories are the datasets used by **A Better
Approximation for Interleaved Dyck Reachability** by Giovanna Kobus Conrado and
Andreas Pavlogiannis. In Lotus they are shared evaluation inputs for Classical
CFG projections/relaxations, the Interleaved-Dyck approximation, MCFL, and
explicitly transformed unary solvers. `InterleavedDyck` names the formal
workload, not a single Lotus implementation.

Each input is a DOT graph whose edges use these labels:

- `op--N` / `cp--N`: opening/closing parenthesis of type `N`;
- `ob--N` / `cb--N`: opening/closing bracket of type `N`; and
- `normal`: an unconstrained value-flow edge.

The supplied artifact contains no license file. Keep that provenance in mind when redistributing the datasets.

## Shared Lotus input

Load these files with `interleaved_dyck::Graph::parseDotFile` from
`CFL/InterleavedDyck/Core/Graph.h`. The same parsed graph can be passed directly
to:

- `interleaved_dyck::staged_bounds::Solver`; and
- `interleaved_dyck::mcfl::InterleavedDyckSolver`, through its typed graph adapter.

The corpus is directed and therefore does not satisfy the exact-mode input
contract of `interleaved_dyck::unary::FixedCounterSolver` or
`interleaved_dyck::unary::AdaptiveSolver`. Their default policy rejects it. The
explicit `AddMissingReverseEdges`/`--bidirect` policy computes a sound
overapproximation for the original directed graph. Experiments must report
that transformation because it changes the reachability problem.
