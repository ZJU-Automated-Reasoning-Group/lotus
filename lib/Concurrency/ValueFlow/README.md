# Thread-aware sparse value-flow refinement

This directory contains the optional whole-program refinement used by the
concurrency checker:

- `ThreadAwareSVFGBuilder` adds guarded Store-to-Load and symmetric
  Store-to-Store `ThreadMHPIndirectVF` edges for accesses that may run in
  parallel. A `FilteredSVFGView` can restrict construction to an induced
  subgraph without copying graph nodes.
- `SparseValueFlowRefinement` is a lightweight concurrency-specific diagnostic
  refinement. It is not the alias-analysis oracle and cannot suppress race
  candidates.
- `Alias/InclusionBased/FlowSensitive/FlowSensitivePTA` owns the general
  top-level points-to and per-node MemorySSA `IN/OUT` state. `FSMPTA` composes
  that solver with the thread-aware SVFG and sliced solve scope.
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
  --concur.memory-partition=inter-disjoint \
  --concur.points-to-sets=hash-consed input.bc
```

The sparse analysis and hash-consed backend are both opt-in. Mutable ordered
sets remain the default. Incomplete or wildcard points-to results never
suppress a race candidate; the checker only removes a pair when complete
sparse results prove its access-target sets disjoint.
