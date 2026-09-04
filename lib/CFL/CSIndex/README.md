# Context-Sensitive Reachability Indexing

`CSIndex` contains two related systems with a one-way dependency:

```text
SCS -> FLARE
```

The public headers under `include/CFL/CSIndex` mirror this directory.

## FLARE

`FLARE` contains the OOPSLA 2022 extended-Dyck indexing implementation and its
baseline query algorithms.

| Path | Responsibility |
|---|---|
| `FLARE/Graph.*` | Labeled input graph, summary edges, and the two-layer indexing transformation |
| `FLARE/GraphAlgorithms.*` | SCC, DAG, traversal, gate-graph, and labeling algorithms |
| `FLARE/Index.*` | Backbone-based reachability index and materialization |
| `FLARE/ReachBackbone.*` | Backbone discovery and compressed graph construction |
| `FLARE/Grail/` | GRAIL index, exception lists, and transitive-closure estimation |
| `FLARE/PathTree/` | PathTree index, weighted path graphs, and label compression |
| `FLARE/Tabulation/` | Sequential and parallel exact baselines |

The public namespace is `lotus::cfl::cs_index::flare`. Algorithm-specific
namespaces are `flare::grail`, `flare::path_tree`, and `flare::tabulation`.

## SCS

`SCS` is the sanitizer-aware extension. It composes an edge-event policy
automaton with the structural call/return graph before applying FLARE.

| Header | Responsibility |
|---|---|
| `SCS/Graph.h` | Edge-identity graph with independent structural and event labels |
| `SCS/PolicyAutomaton.h` | DFA or epsilon-free NFA policy |
| `SCS/Index.h` | Explicit/lazy products, point and batch queries, metrics, and witnesses |
| `SCS/FactorizedIndex.h` | Disjunctive composition of independently maintained indexes |

The public namespace is `lotus::cfl::cs_index::scs`. SCS depends on FLARE's
graph transformation and GRAIL index; FLARE has no SCS dependency.

## Build targets

| Target | Contents |
|---|---|
| `CanaryCSIndexFLARE` | FLARE, GRAIL, PathTree, and tabulation |
| `CanaryCSIndexSCS` | SCS, linking `CanaryCSIndexFLARE` |

The optional `csr` command links only `CanaryCSIndexFLARE`. The former
`CFLCallingContextSolver` and duplicate `CSProgressBar` were unused and are not
part of the reorganized API; FLARE uses `Utils/Platform/ProgressBar`.
