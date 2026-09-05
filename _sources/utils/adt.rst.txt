Utility ADTs and Worklists
==========================

``include/Utils/ADT/`` provides the reusable container and worklist layer used
across analyses.

**Main components**:

- ``DisjointSet`` and ``UnionFind`` for partition maintenance.
- ``ImmutableMap``, ``ImmutableSet``, and ``ImmutableTree`` for persistent data.
- ``PriorityWorkList`` and ``TwoLevelWorkList`` for solver scheduling.
- ``TreeStream`` and related iterator adapters for structured traversal.
- ``ewah`` — Compressed bitmap (EWAH) for memory-efficient set representation,
  commonly used in solver and dataflow contexts.

These headers are used heavily by the dataflow, alias, and solver subsystems.

Choosing a worklist
-------------------

Use a priority worklist when an analysis has a meaningful ordering that can
accelerate convergence, and use a two-level worklist when it needs separate
queues for coarse and fine-grained work.  Persistent containers are useful for
sharing immutable facts across states; avoid them in a hot mutation loop unless
their structural sharing outweighs the update cost.

See also :doc:`utilities`.
