Parallel Execution Infrastructure
=================================

``include/Utils/Parallel/`` and ``lib/Utils/Parallel/`` provide the shared
thread-pool and scheduling utilities used by parallel analyses.

**Main components**:

- ``ThreadPool`` for task execution and parallel loops.
- ``CancellationToken`` and ``CancellationSource`` for cooperative stop.
- ``PipelineScheduler`` and ``ParallelSchedulerPass`` for structured pipelines.
- ``Task`` and thread-local reducer helpers for work coordination.

Use this layer when an analysis needs bounded parallelism without embedding its
own scheduler.

Concurrency guidance
--------------------

Tasks should communicate cancellation through the provided token rather than
abandoning shared work abruptly.  Make ownership and mutation boundaries
explicit: LLVM IR objects and analysis caches are not automatically safe for
concurrent writes.  Use reducer helpers for accumulations, then merge results
after parallel work completes to preserve deterministic reporting where it
matters.

See also :doc:`utilities`.
