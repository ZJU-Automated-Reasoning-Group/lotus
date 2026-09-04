# Interleaved-Dyck Graph Reduction

This module packages the graph-simplification pipeline from Yuanbo Li, Qirun
Zhang, and Thomas Reps, *Fast Graph Simplification for Interleaved
Dyck-Reachability* (PLDI 2020).

It is a graph transformation, not a reachability solver. Its output is a
smaller DOT graph intended to preserve the reachability property covered by
the reduction theorem. A reduced graph must still be analyzed by a compatible
downstream solver.

## Input and bidirected semantics

The driver consumes the same `op--N`, `cp--N`, `ob--N`, `cb--N`, and `normal`
DOT format used by `Core`. It edits a caller-supplied working
copy in place.

The shared input graph is never implicitly mutated by `Core`,
Approximation, or MCFL. This reducer has specialized internal orientation
rules: closing colored edges are represented in reverse orientation, and the
legacy one-color CFL construction can create synthetic reverse terminals for
matching. Those edges are internal summaries, not an assertion that the
original input was bidirected. Use `--bidirected-input` only when both
directions are already represented by the input.

## Build and run

Build the driver and its two compiled helpers:

```sh
cmake --build build --target lotus-cfl-interleaved-dyck-graph-reduction
```

Copy a graph before simplifying because the Python driver updates it in place:

```sh
cp input.dot reduced.dot
python3 build/bin/lotus-cfl-interleaved-dyck-graph-reduction.py reduced.dot \
  --graphaux build/bin/lotus-cfl-interleaved-dyck-graphaux \
  --dkmerge build/bin/lotus-cfl-interleaved-dyck-dkmerge
```

The driver now accepts explicit paths and no longer depends on a local
Makefile, `dotfile/exp-2020`, or binaries in the current directory.

## Code organization

- `CanaryInterleavedDyckGraphReduction` contains the low-level adjacency implementation;
  private fast-list and summary types live under `Legacy/`.
- `GraphAux.cpp` performs one-color component construction.
- `dkMerge.cpp` performs the degree-based merge phase.
- `graph_simp.py` alternates both colors and removes proven-redundant edges.
- `Legacy/` contains private artifact-era data structures. They are not a
  stable public C++ API and no longer occupy `include/CFL`.

The specialized reducer representation is intentionally not merged with
`interleaved_dyck::Graph`: it stores intermediate merge classes, colored
summary edges, degrees, and cancellation bookkeeping rather than just the
input graph. The file-level boundary allows it to consume the same datasets
without pretending the internal representations are interchangeable.
