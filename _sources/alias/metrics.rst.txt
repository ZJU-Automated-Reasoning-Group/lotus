Pointer Analysis Metrics
========================

``include/Alias/Infrastructure/Metrics/`` and ``lib/Alias/Infrastructure/Metrics/`` provide helpers for
measuring precision and soundness-related properties of alias analyses.

**Main components**:

- ``PointerAnalysisMetrics`` stores collected statistics.
- ``CollectMetricsOptions`` configures metric collection.
- ``collectMetricsFromWrapper`` runs the collection against an alias-analysis
  wrapper.

This module is primarily for evaluation, comparison, and regression tracking of
points-to algorithms.

Collected measurements
----------------------

The collector records points-to-set size statistics when a wrapper exposes
them, including the number of tracked pointers, total, maximum, average, and
median set size.  It also records direct and resolved indirect call-graph
edges, the number of polymorphic indirect call sites, and the average number
of targets per indirect call.

Optionally, it samples alias queries over pointer use-site pairs.  The result
separates ``NoAlias``, ``MustAlias``, ``MayAlias``, and ``PartialAlias``
answers.  ``PointerAnalysisMetrics::fractionNoAlias`` is a compact indication
of how many sampled pairs the analysis can prove disjoint; it is most useful
when compared across runs using the same module and query limit.

Cost and interpretation
-----------------------

Points-to and call-graph counts are inexpensive summaries of an already-run
analysis.  Pairwise alias sampling can be much more costly, so
``CollectMetricsOptions::max_alias_pairs`` caps it (the default is 50,000).
Set this value to zero to omit pair metrics, or use a fixed nonzero value for
comparable regression results.

These are comparative metrics, not a proof of soundness.  Lower points-to
counts or fewer indirect targets commonly indicate greater precision, but the
comparison is meaningful only when analyses model the same program, libraries,
and external-call assumptions.

See also :doc:`alias_analysis`.
