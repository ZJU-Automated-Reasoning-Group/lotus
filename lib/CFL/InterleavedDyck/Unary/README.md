# Exact Unary Interleaved Dyck Reachability

`CanaryInterleavedDyckUnary` contains exact algorithms for bidirected unary
`D1`-interleaved-`D1` reachability. The directory names the problem scope;
algorithm names live below that scope.

| API | Algorithm | Finite control after preprocessing |
|---|---|---|
| `AdaptiveSolver` | Adaptive two-arm construction | `O(n^2)` states |
| `FixedCounterSolver` | Kjelstrøm–Pavlogiannis POPL 2022, Algorithm 1 | `O(n^3)` states |

Both algorithms consume `interleaved_dyck::Graph` and share unary projection,
input validation, quotient sparsification, and the bidirected one-counter
component backend from `CFL/InterleavedDyck/Core`. This makes their comparison
about finite-control construction rather than parser or preprocessing choices.

## Exactness boundary

Exactness requires:

- every projected arc has its complement-labeled reverse arc;
- every parenthesis ID operates on counter 1 and every bracket ID on counter 2;
- queries ask for balanced paths between zero-counter configurations.

The module does not claim exactness for multi-type
`D_k`-interleaved-`D_k` reachability.

Both option types expose `input_policy`:

- `RequireBidirected` is the default. It rejects a missing complement reverse
  arc and remains exact for the supplied graph.
- `AddMissingReverseEdges` explicitly symmetrizes the graph. The result is
  exact for the symmetrized graph and a sound overapproximation for the
  original directed graph. Result statistics record the inserted arcs and
  weaker guarantee.

The shared parser never adds reverse arcs implicitly.

## Algorithms

### Adaptive

`AdaptiveSolver::solveShallow(graph, K)` computes exactly the
zero-configuration partition inside
`X_K = {(v,a,b) : min(a,b) <= K}`. It constructs two finite-control
one-counter arms, computes their zero-height components, derives positive
height labels through a parent map, and merges both arm partitions at their
boundary.

`AdaptiveSolver::solve(graph)` first applies the fixed-alphabet quotient,
chooses `K = 6 * |V(quotient)|`, computes the shallow partition, and lifts it
to the input vertices. Set `AdaptiveOptions::sparsify` to `false` for the
direct construction with `K = 6 * |V(input)|`.

### Fixed counter (POPL 2022)

For a processed graph with `n` vertices, `FixedCounterSolver` uses

```text
C = 18*n^2 + 6*n.
```

It constructs states `(v,j)` for every `0 <= j <= C`, storing counter 2 in
`j`. Counter-2 labels become epsilon transitions that change `j`; counter-1
labels remain the opening/closing labels of a bidirected one-counter graph.
Vertices `u` and `v` are connected exactly when `(u,0)` and `(v,0)` are in the
same zero-height component.

The backend stores closing and epsilon edges explicitly and obtains matching
opening edges from bidirectedness. This is a representation optimization of
Algorithm 1, not a semantic change. Set `FixedCounterOptions::sparsify` to
`false` for the literal construction on the supplied graph.

## C++ API

```cpp
#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Unary/Solver.h"

using namespace lotus::cfl;

interleaved_dyck::Graph graph =
    interleaved_dyck::Graph::parseDotFile("bidirected.dot");
auto adaptive = interleaved_dyck::unary::AdaptiveSolver{}.solve(graph);
auto fixed = interleaved_dyck::unary::FixedCounterSolver{}.solve(graph);
```

## Command line

One tool selects either algorithm:

```sh
cmake --build build --target lotus-cfl-interleaved-dyck-unary
build/bin/lotus-cfl-interleaved-dyck-unary \
  --algorithm adaptive --stats bidirected.dot
build/bin/lotus-cfl-interleaved-dyck-unary \
  --algorithm fixed-counter --stats bidirected.dot
```

`--direct` disables quotient sparsification. `--shallow K` is specific to the
adaptive algorithm. `--print-pairs` materializes the component relation.
`--bidirect` explicitly selects `AddMissingReverseEdges`; output always states
the selected algorithm and exactness guarantee.

## Benchmark use

The algorithms can parse the datasets under
`benchmarks/real-world/CFL/InterleavedDyck`, but that published corpus is
directed. The default policy therefore rejects it. Experiments may explicitly
use `--bidirect` as a sound overapproximation, but must report that graph
transformation, or use a genuinely bidirected corpus for exact comparisons.

## Validation and fidelity

Tests cover shallow thresholds, repeated arm switching, cascading component
merges, random and exhaustive small graphs, exact fixed-counter construction
sizes, quotient equivalence, and directed-input policies.

`FixedCounterSolver` faithfully implements the exact bounded-path construction
of POPL 2022 Algorithm 1 and the ordinary-Dyck sparsity reduction. The paper's
experimental doubly-self-looped-node removal and motif trimming are not
included. Controlled comparisons deliberately give both algorithms the same
shared preprocessing; reproducing the artifact's wall-clock table would
require exposing those heuristics as a separate experimental configuration.
