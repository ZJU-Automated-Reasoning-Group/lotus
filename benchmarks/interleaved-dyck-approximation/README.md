# Interleaved-Dyck approximation datasets

The `taint/` and `valueflow/` directories are the datasets
used by **A Better Approximation for Interleaved Dyck Reachability** by Giovanna Kobus Conrado and Andreas Pavlogiannis.

Each input is a DOT graph whose edges use these labels:

- `op--N` / `cp--N`: opening/closing parenthesis of type `N`;
- `ob--N` / `cb--N`: opening/closing bracket of type `N`; and
- `normal`: an unconstrained value-flow edge.

The supplied artifact contains no license file. Keep that provenance in mind when redistributing the datasets.

## Shared Lotus input

Load these files with `interleaved_dyck::Graph::parseDotFile` from
`CFL/InterleavedDyckCore/Graph.h`. The same parsed graph can be passed directly
to:

- `interleaved_dyck_approximation::Solver`; and
- `mcfl::InterleavedDyckSolver`, through its typed graph adapter.

The corpus is directed and therefore does not satisfy the input contract of
the exact mode of
`adaptive_interleaved_dyck::AdaptiveInterleavedDyckSolver`. The default policy
rejects it. The explicit `AddMissingReverseEdges`/`--bidirect` policy computes
a sound overapproximation for the original directed graph. Experiments must
report that transformation because it changes the reachability problem.
