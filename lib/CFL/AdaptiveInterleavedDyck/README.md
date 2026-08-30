# Exact Adaptive Interleaved Dyck

`CanaryAdaptiveInterleavedDyck` implements exact all-pairs reachability for
bidirected unary `D1`-interleaved-`D1` graphs using adaptive counter
flattening. It is independent of MCFL and consumes the shared typed graph from
`CFL/InterleavedDyckCore`.

## Exactness boundary

Exactness requires:

- every input arc has its complement-labeled reverse after unary projection;
- all parenthesis IDs project to counter 1 and all bracket IDs to counter 2;
- queries concern balanced paths between zero-counter configurations.

The solver does not claim exactness for multi-type
`D_k`-interleaved-`D_k`.

`AdaptiveInterleavedOptions::input_policy` makes directed-input handling
explicit:

- `RequireBidirected` is the default. It rejects a missing complement reverse
  arc and keeps the result exact for the original graph.
- `AddMissingReverseEdges` adds complement-labeled reverse arcs. The result is
  exact for the symmetrized graph and a sound overapproximation for the
  original directed graph. Statistics record inserted arcs and the guarantee.

The shared parser never adds reverse arcs implicitly.

## APIs

`AdaptiveInterleavedDyckSolver::solveShallow(graph, K)` computes exactly the
zero-configuration partition inside
`X_K = {(v,a,b) : min(a,b) <= K}`. It constructs two finite-control
one-counter arms, computes their zero-height components, derives positive
height labels through the parent map, and merges both arm partitions at their
boundary.

`AdaptiveInterleavedDyckSolver::solve(graph)` runs the complete algorithm. It
sparsifies the fixed-alphabet graph, chooses `K = 6 * |V(quotient)|`, computes
the shallow partition, and lifts component identifiers to the input vertices.
Set `AdaptiveInterleavedOptions::sparsify` to `false` for the direct
`K = 6 * |V(input)|` construction.

## Command line

Build and run the independent tool with:

```sh
cmake --build build --target lotus-cfl-adaptive-interleaved-dyck
build/bin/lotus-cfl-adaptive-interleaved-dyck --stats bidirected.dot
```

Use `--direct` to skip quotient sparsification or `--shallow K` to solve a
caller-selected shallow region. `--print-pairs` materializes the component
relation. `--bidirect` explicitly selects `AddMissingReverseEdges`; the CLI
always prints whether its result is exact or overapproximate.

## Benchmark use

The solver can load any file from `benchmarks/interleaved-dyck-approximation`
through the shared parser, but it runs only when the loaded graph is
bidirected under the default policy. The published approximation corpus is
directed, so the default rejects it. A comparison experiment may select
`--bidirect` as a sound overapproximation, but must report that transformation
because it changes the analyzed graph.

## Validation

Focused tests cover shallow thresholds, repeated arm switching, cascading
self-edge merges, randomized shallow graphs, and every subset of a fixed
adversarial edge universe checked against the prior `18n^2 + 6n` bounded
construction.
