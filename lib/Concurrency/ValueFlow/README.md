# Thread-aware sparse value-flow refinement

This directory contains the optional whole-program refinement used by the
concurrency checker:

- `ThreadAwareSVFGBuilder` adds guarded Store-to-Load and symmetric
  Store-to-Store `ThreadMHPIndirectVF` edges for accesses that may run in
  parallel. A `FilteredSVFGView` can restrict construction to an induced
  subgraph without copying graph nodes.
- `SparseFlowSensitivePTA` propagates pointer values through the SVFG and keeps
  memory values on MemorySSA definitions. Sequential singleton-global stores
  use strong updates; thread-interference inputs are always weak updates.
- `WholeProgramSparseRefinement` owns the ICFG, SVFG, overlay, and solver for a
  complete analysis run.
- `MultiStageSlicer` implements the MSli pipeline: candidate closure,
  synchronization/call expansion, and a bounded PTA closure. The pre-analysis
  overlay is discarded; MHP and lock queries are evaluated again while
  constructing the filtered main-phase overlay.
- `ThreadCallGraph` and `ThreadCreationTree` rebuild context-bounded thread
  instances for both the pre-analysis and sliced main phase. Forks in loops or
  recursive contexts are marked multi-instance, and joins are resolved through
  canonicalized thread handles.

Thread-aware MemorySSA treats a fork as forward-only and a join as
return-only: caller memory definitions and payloads flow into the start
routine, while joined `FormalOut` definitions flow to synthetic `ActualOut`
nodes and dominated post-join accesses.

MemorySSA construction supports three frozen partition policies:

- `distinct`: preserve each exact points-to set;
- `intra-disjoint`: merge overlapping sets independently per function;
- `inter-disjoint`: merge overlapping sets over the whole module.

Freezing the partition before MemorySSA construction prevents overlapping
points-to sets from silently receiving unrelated SSA version streams.

The checker enables this path with:

```text
lotus-check --engine=concur --checks=data-race \
  --concur.sparse-flow-sensitive input.bc
```

Multi-stage slicing is enabled with:

```text
lotus-check --engine=concur --checks=data-race --concur.msli \
  --concur.memory-partition=inter-disjoint input.bc
```

The option is off by default while performance and precision are evaluated on
larger programs. Incomplete or wildcard points-to results never suppress a
race candidate; the checker only removes a pair when complete sparse results
prove its access-target sets disjoint.
