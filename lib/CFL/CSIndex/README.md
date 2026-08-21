# CSIndex: Indexing Context-Sensitive Reachability

## Overview

This module implements efficient indexing and querying for **Extended Dyck-CFL (Context-Free Language) Reachability** in context-sensitive program analysis. The system provides fast reachability queries over large program graphs while respecting inter-procedural control flow constraints.

**Reference:** OOPSLA 2022a - "Indexing the Extended Dyck-CFL Reachability for Context-Sensitive Program Analysis"  
Qingkai Shi, Yongchao Wang, Peisen Yao, and Charles Zhang.  
The 37th ACM SIGPLAN Conference on Object-Oriented Programming, Systems, Languages, and Applications.

## Key Concepts

### CFL Reachability
- **Positive labels**: Represent call edges (entering functions)
- **Negative labels**: Represent return edges (exiting functions)  
- **Unlabeled edges**: Represent intra-procedural flow
- **Valid paths**: Must respect matched call-return pairs (Dyck language grammar)

### Core Components

1. **Graph (Graph.cpp)**
   - Graph data structure with labeled edges for CFL reachability
   - Summary edge computation for inter-procedural reachability
   - Indexing graph transformation (bipartite structure)

2. **ReachBackbone (ReachBackbone.cpp)**
   - Discovers minimal set of "backbone" vertices (gates) for indexing
   - Uses life propagation algorithm to determine necessary vertices
   - Builds compressed gate graph over backbone vertices

3. **Query (Query.cpp)**
   - Multiple query strategies: DFS, GATEDFS, GRAIL, Index-based
   - Materialization for high-degree vertices
   - Local gate precomputation for fast queries

4. **Tabulation (Tabulation.cpp, ParallelTabulation.cpp)**
   - Sequential and parallel CFL reachability algorithms
   - Used for correctness validation and TC estimation
   - Respects call-return matching constraints

5. **DataComp (DataComp.cpp)**
   - Clustering-based compression for index storage
   - K-means and sliding window heuristics
   - Reduces storage while maintaining fast decompression

6. **DWGraphUtil (DWGraphUtil.cpp)**
   - Edmonds' maximum branching algorithm
   - Tarjan's SCC algorithm
   - Graph optimization utilities

## Architecture

```
Input Graph (with labeled edges)
    ↓
[ReachBackbone] → Discover backbone vertices → Build gate graph
    ↓
[Query] → Load indices → Answer reachability queries
    ↓
[Tabulation] → Validate correctness / Estimate TC size
```

## Usage Workflow

1. **Graph Construction**: Build graph with labeled edges from program analysis
2. **Backbone Discovery**: Run `ReachBackbone::backboneDiscovery()` to find gates
3. **Index Building**: Generate gate graph and compute reachability indices
4. **Query Processing**: Use `Query::reach()` for fast reachability queries
5. **Validation**: Use `Tabulation::reach()` to verify correctness

## Sanitizer-Aware Context-Sensitive Reachability

`SCSIndex` composes a finite-state security policy with FLARE. Each input edge
has two independent projections:

- a structural label (`0`, positive call, or negative return), and
- a security-event label (`0` means no event).

The implementation builds the automaton product first, preserving structural
labels, and then applies the existing summary-edge and two-layer FLARE
transformation. This ensures that sanitizer semantics and call/return matching
refer to the same witness path.

The public entry points are:

- `SCSGraph`: edge-identity graph supporting dual-role and parallel edges;
- `PolicyAutomaton`: total DFA or epsilon-free NFA policy;
- `SCSIndex`: explicit or source-rooted lazy products, point queries, fixed
  batch endpoints, GRAIL queries, metrics, and optional witness replay;
- `FactorizedSCSIndex`: disjunctive composition for independent categories.

Existing `Graph`, `csr`, and FLARE query behavior is unchanged. Summary-path
provenance is recorded only when `SCSIndexOptions::retain_witnesses` is enabled.

`SCSIndex::stats()` exposes evaluation counters without printing from the
library. Construction metrics include base/product/FLARE/index/DAG sizes and
nanosecond timings for validation, product construction, graph copying,
summary construction, FLARE transformation, endpoint augmentation, SCC
condensation, and GRAIL construction. Point, fixed-batch, and witness queries
record their count, positive count, cumulative latency, and maximum latency.
Raw times use nanoseconds so benchmark drivers can choose their own reporting
unit; convenience methods provide construction milliseconds, average query
microseconds, and the fraction of the explicit product materialized.
`writeCsvHeader()` and `writeCsvRow()` emit a stable, one-row-per-run format for
benchmark scripts. On POSIX systems the row also includes process-wide peak RSS
before and after construction; run each benchmark configuration in a fresh
process when using those high-water marks for comparisons.

## File Structure

- **Graph.cpp**: Core graph data structure and CFL operations
- **ReachBackbone.cpp**: Backbone discovery algorithm
- **Query.cpp**: Query interface and indexing strategies
- **Tabulation.cpp**: Sequential CFL reachability
- **ParallelTabulation.cpp**: Parallel CFL reachability
- **DataComp.cpp**: Index compression algorithms
- **DWGraphUtil.cpp**: Graph algorithms (Edmonds, Tarjan)
- **GraphUtil.cpp**: General graph utilities (DFS, topological sort)
- **CSProgressBar.cpp**: Progress visualization

## Algorithm Highlights

### Backbone Discovery
- Pre-selects high-degree vertices
- Materializes local neighborhoods for hubs
- Uses BFS with life propagation to identify necessary gates
- Builds compressed gate graph for efficient queries

### Query Strategies
- **DFS**: Baseline depth-first search
- **GATEDFS**: Gate-based DFS with local gate caching
- **Index-based**: Precomputed lin/lout labels for O(1) filtering
- **GRAIL**: Multi-labeling for probabilistic quick rejection

### Compression
- Groups similar index vectors into clusters
- Stores common elements once in compression table
- Vectors store only differences from cluster centroids
- Achieves significant space savings for large indices

## Performance Characteristics

- **Index size**: Typically 10-50x smaller than full transitive closure
- **Query time**: Sub-millisecond for most queries using indexing
- **Construction time**: O(n * m) for backbone discovery, parallelizable
- **Memory**: Adaptive materialization balances speed vs. memory

## Notes

- The system is designed for large-scale program analysis (millions of vertices)
- Parallel tabulation provides significant speedup on multi-core systems
- Compression is most effective when many vertices share common reachability patterns
