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

The checker enables this path with:

```text
lotus-check --engine=concur --checks=data-race \
  --concur.sparse-flow-sensitive input.bc
```

The option is off by default while performance and precision are evaluated on
larger programs. Incomplete or wildcard points-to results never suppress a
race candidate; the checker only removes a pair when complete sparse results
prove its access-target sets disjoint.
