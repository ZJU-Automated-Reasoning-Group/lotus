VASCO Value-Context Framework
=============================

Overview
========

The **VASCO** framework in ``include/Dataflow/VASCO`` and
``lib/Dataflow/VASCO`` implements the value-context interprocedural data-flow
algorithm of Padhye and Khedker, *Interprocedural Data Flow Analysis in Soot
using Value Contexts* (SOAP 2013).

* **Location**: ``include/Dataflow/VASCO``, ``lib/Dataflow/VASCO``
* **Purpose**: precise, flow-sensitive, context-sensitive interprocedural
  analysis for finite lattices, including non-distributive problems

Why VASCO
=========

Lotus already contains specialized interprocedural engines such as IFDS/IDE and
WPDS. VASCO serves a different niche.

VASCO is appropriate when:

* the analysis needs **valid-path**, context-sensitive propagation,
* transfer functions are **monotone** but not necessarily **distributive**,
* the abstract domain is a **finite lattice**, and
* recursive procedures must still be handled precisely.

This is exactly the setting emphasized in the SOAP 2013 paper: analyses such as
sign analysis or points-to analysis often cannot be encoded cleanly as IFDS/IDE
because a transfer function depends on the whole abstract state, not on one fact
at a time.

Value Contexts
==============

The paper's key idea is to use a data-flow value itself as the calling context.

For a forward analysis, a value context is:

.. math::

   X = \langle m, entry\_value \rangle

where ``m`` is a procedure and ``entry_value`` is the lattice value at the
procedure entry.

For backward analyses, Lotus uses the dual notion and keys contexts by exit
values instead. In both cases, if two calls reach the same procedure with the
same context value, they reuse the same context and therefore the same computed
summary.

This combines two ideas highlighted in the paper:

* the tabulation perspective of representing a procedure summary by an
  input/output pair of lattice values, and
* value-based termination of call-string construction, which avoids the
  exponential blow-up of raw call strings.

Algorithm Sketch
================

Lotus's forward and backward solvers directly mirror Figure 1 of the paper.

For a forward analysis, the solver:

* creates initial contexts for all entry points using ``boundaryValue()``,
* processes CFG nodes in a per-context worklist,
* computes node IN values as the meet of predecessor OUT values,
* treats calls specially by:

  * computing a callee entry value,
  * looking up or creating the matching callee context,
  * recording the caller-context/call-site to callee-context transition,
  * reusing the callee exit value when that context has already been analyzed,

* re-enqueues successors when a node's output improves, and
* re-enqueues callers when a callee context's exit summary changes.

The backward solver is the same idea with entry/exit roles reversed.

Termination follows the same argument as in the paper: monotone transfer
functions over a finite lattice yield a finite descending chain, which also
bounds the number of distinct value contexts.

Core API
========

Generic Interfaces
------------------

The framework is generic in three types:

* ``M``: method or procedure type,
* ``N``: control-flow graph node type,
* ``A``: abstract data-flow value type.

The core abstractions are:

* ``Core/ProgramRepresentation.h``
  defines entry-point discovery, CFG access, call detection, and call-target
  resolution.
* ``Core/Context.h``
  stores one value context: method, entry value, exit value, per-node IN/OUT
  maps, and a pseudo-topological worklist order.
* ``Core/ContextTransitionTable.h``
  records transitions from a caller context at a call site to the target callee
  context.
* ``Core/InterProceduralAnalysis.h``
  owns all contexts and shared utilities such as
  ``getContexts()`` and ``getMeetOverValidPathsSolution()``.

Directional Solvers
-------------------

Client analyses extend one of:

* ``Solver/ForwardInterProceduralAnalysis.h``
* ``Solver/BackwardInterProceduralAnalysis.h``

They must provide lattice hooks:

* ``topValue()``
* ``boundaryValue(...)``
* ``copy(...)``
* ``meet(...)``

and transfer hooks:

* ``normalFlowFunction(...)``
* ``callEntryFlowFunction(...)``
* ``callExitFlowFunction(...)``
* ``callLocalFlowFunction(...)``

This closely follows the framework section of the paper.

Results
=======

VASCO exposes results in two forms.

Context-sensitive results
-------------------------

Each ``Context`` stores:

* ``getEntryValue()`` and ``getExitValue()`` for the summary of that value
  context,
* ``getValueBefore(node)`` and ``getValueAfter(node)`` for per-node facts inside
  that context.

Merged meet-over-valid-paths results
------------------------------------

``InterProceduralAnalysis::getMeetOverValidPathsSolution()`` merges the results
of all extant contexts for each node. This provides a convenient node-indexed
view while preserving the paper's valid-path semantics.

LLVM Instantiation in Lotus
===========================

Lotus ships an LLVM adaptation of the generic framework.

* ``Adapters/LLVM/DefaultLLVMProgramRepresentation.h``
  adapts LLVM IR to VASCO using instruction-level CFGs. By default it resolves
  direct calls and treats declarations or unresolved callees conservatively. It
  also accepts a custom resolver callback, which is the intended integration
  point for stronger Lotus call-graph information.
* ``Analyses/LLVMSignAnalysis.h``
  ports the paper's sign-analysis example.
* ``Analyses/LLVMCopyConstantAnalysis.h``
  ports copy-constant propagation.
* ``Analyses/LLVMLiveVariablesAnalysis.h``
  adds a backward interprocedural liveness client over LLVM SSA values.
* ``Analyses/LLVMNullnessAnalysis.h``
  tracks whether pointer-valued LLVM SSA results are null, non-null, or
  maybe-null.
* ``Analyses/LLVMPointsToAnalysis.h``
  ports the original VASCO points-to / call-target client to LLVM.

The LLVM points-to client models:

* pointer-like SSA values,
* globals and allocation-site objects,
* field-sensitive constant-offset memory accesses,
* direct calls and indirect calls through function-pointer values.

Parallel Context Scheduling
===========================

Lotus extends the sequential VASCO solver with an opt-in parallel mode that
preserves the original value-context semantics while changing only the order in
which contexts are processed.

Core Idea
---------

Each value context is an independent schedulable task. At most one worker thread
owns a given context at a time, but different contexts can run concurrently on
different threads. The solver's worklist is shared across workers: a thread
acquires a ready context not owned by any other worker, processes up to a
configurable number of local work items under the context's mutex, and then
releases the context back to the shared pool.

Versioned Summary Replay
------------------------

Every context carries an atomic ``SummaryVersion`` that starts at zero and
increments each time the context publishes a changed summary (exit value for
forward analyses, entry value for backward analyses). When a caller context
consumes a summary at a call site, it records the callee's current version in
an ``ObservedSummaryVersions`` table. When the callee publishes an updated
summary, ``wakeStaleCallers()`` iterates the caller list and re-enqueues only
those call sites whose observed version is older than the current callee
version (or that have no observation yet). This avoids waking callers that have
already seen the latest summary.

Public API
----------

The following methods on ``InterProceduralAnalysis`` control parallel
scheduling:

* ``setParallelContextScheduling(bool Enable)``
  enables or disables parallel context scheduling (default: ``false``).

* ``setContextStepBudget(size_t Budget)``
  sets the maximum number of work items processed per context batch (default:
  ``64``; a value of ``0`` is clamped to ``1``).

* ``getSchedulerOptions()``
  returns a ``SchedulerOptions`` struct describing the current configuration.

* ``getSchedulerStats()``
  returns a ``SchedulerStats`` struct holding cumulative counter values.

SchedulerOptions
^^^^^^^^^^^^^^^^

.. code-block:: cpp

   struct SchedulerOptions {
     bool EnableParallelContextScheduling = false;
     std::size_t ContextStepBudget = 64;
   };

SchedulerStats
^^^^^^^^^^^^^^

The ``SchedulerStats`` struct exposes 11 counters:

* ``contexts_created`` — number of new value contexts interned
* ``contexts_reused`` — number of existing contexts reused by a caller
* ``context_steps`` — total work items processed across all contexts
* ``context_batches`` — total context-batch invocations
* ``caller_wakeups`` — times a caller's call node was re-enqueued
* ``summary_publications`` — times a changed summary triggered publication
* ``suppressed_unchanged_wakeups`` — times caller wakeup was suppressed because
  the summary did not change
* ``stale_callsite_replays`` — times a stale caller call site triggered replay
* ``current_callsite_replays_skipped`` — times a current-version call site
  skipped replay
* ``parallel_worker_tasks`` — worker tasks launched in the thread pool
* ``max_ready_contexts`` — peak size of the ready-context worklist

Configuration
-------------

Parallel scheduling uses the ``ThreadPool`` singleton configured with the
``-nworkers=N`` command-line flag. The solver checks three conditions before
entering parallel mode:

1. ``EnableParallelContextScheduling`` is ``true``,
2. ``FreeResultsOnTheFly`` is ``false``, and
3. ``ThreadPool::get()->hasWorkers()`` returns ``true``.

If any condition fails, the solver falls back to the original sequential loop.
Setting ``-nworkers=0`` effectively disables parallelism even when the option
is enabled.

Usage Example
-------------

.. code-block:: cpp

   analysis.setParallelContextScheduling(true);
   analysis.setContextStepBudget(128);
   analysis.doAnalysis();

   auto Stats = analysis.getSchedulerStats();
   llvm::outs() << "Parallel batches: " << Stats.context_batches << "\n";

Correctness Invariants
----------------------

The parallel scheduler relies on these invariants to guarantee that the final
solution matches the sequential solver:

1. Only one worker mutates a context at a time (per-context
   ``recursive_mutex``).
2. Context interning by ``(method, value)`` is atomic (shared ``StateMutex``).
3. Data-flow values evolve through the existing monotone ``meet`` logic.
4. A changed summary atomically increments its context version.
5. Any caller that consumed an older summary is eventually replayed.
6. A call site that already consumed the current summary does not need replay.
7. The final meet-over-valid-paths solution matches the sequential solver.

Design Document
---------------

A standalone design document describing the scheduling algorithm, state
organization, and implementation details can be found at:

   ``lib/Dataflow/VASCO/ParallelContextScheduler.md``

The test harness in ``tests/unit/Dataflow/VASCO/VASCOParallelHarnessTest.cpp``
exercises both sequential (``-nworkers=0``) and parallel (``-nworkers=2``)
modes and verifies that they produce identical results.

Current Scope and Caveats
=========================

The framework implementation is faithful to the paper's value-context solver,
but the LLVM instantiation is deliberately lightweight.

* Default call resolution is strongest for direct calls.
* Unresolved or external calls are handled conservatively.
* The points-to client is field-sensitive for constant offsets, but remains
  conservative around unknown offsets, advanced heap modeling, and external
  library summaries.
* Clients needing richer indirect-call precision should provide a custom
  ``ProgramRepresentation`` or custom target resolver rather than relying only
  on the default LLVM adapter.

Relationship to Other Lotus Engines
===================================

Use VASCO when the analysis needs a general finite-lattice solver with precise
interprocedural valid-path handling, especially for **non-distributive** flow
functions.

Use :doc:`ifds_ide` when the problem fits the IFDS or IDE restrictions and you
want the specialized exploded-supergraph engine. Use :doc:`wpds` when a
pushdown-system formulation is the better fit.
