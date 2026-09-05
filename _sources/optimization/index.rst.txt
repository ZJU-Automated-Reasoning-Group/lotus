Optimization
============

This section documents the optimization libraries under ``lib/Optimization/``.
They complement the transformation utilities in :doc:`../transform/index` and
provide both standalone passes and reusable integration helpers.

**Headers**: ``include/Optimization/``

**Implementation**: ``lib/Optimization/``

Subdirectories
--------------

- ``PassOrdering/`` exposes a small API for running LLVM's default O0-O3 pass ordering.
- ``Scalar/`` contains LLVM-style scalar passes such as aggressive inlining,
  dead-store elimination, GVN, and LICM.
- ``IPO/`` contains interprocedural optimizations built on SeaDsa ShadowMem and
  Lotus MemorySSA instrumentation.
- ``PartialEvaluation/`` contains the LLPE specialization and partial
  evaluation subsystem.
- ``Prefetch/`` contains the profile-guided software prefetching pass.

ModuleOptimizer
---------------

**Files**: ``include/Optimization/PassOrdering/ModuleOptimizer.h``,
``lib/Optimization/PassOrdering/ModuleOptimizer.cpp``

``llvm_utils::optimiseModule`` runs LLVM's default per-module optimization
pipeline using ``llvm::PassBuilder`` and an ``llvm::OptimizationLevel``.

.. code-block:: cpp

   #include <Optimization/PassOrdering/ModuleOptimizer.h>

   llvm::Module *M = ...;
   llvm_utils::optimiseModule(M, llvm::OptimizationLevel::O2);

Use it when a tool wants standard LLVM cleanup before running Lotus analyses or
custom optimization passes.

Scalar passes
-------------

``lib/Optimization/Scalar/`` currently builds ``CanaryOptimizationScalar`` from
four passes:

- ``AggressiveInliner`` (legacy pass name ``ainline``)
- ``DSE`` / ``createDeadStoreEliminationPass()``
- ``GVN`` / ``createGVNPass()``
- ``LICM`` / ``createLICMPass()``

See :doc:`scalar` for the pass-level details and available entry points.

Interprocedural MemorySSA optimizations
---------------------------------------

``lib/Optimization/IPO/`` contains four legacy ``ModulePass`` implementations
that operate on ShadowMem-instrumented IR:

- ``IPDeadStoreElimination`` removes stores and some global initializers whose
  ShadowMem def-use chains never reach an observable load.
- ``IPRedundantLoadElimination`` removes repeated loads within a block when the
  pointer and TLVar are unchanged.
- ``IPStoreSinking`` moves stores closer to their first observable use within a
  basic block.
- ``IPStoreToLoadForwarding`` replaces loads with a unique reaching store value
  found by traversing MemorySSA edges across calls and phis.

See :doc:`ip` for prerequisites, pass names, and behavior.

Partial evaluation
------------------

``lib/Optimization/PartialEvaluation/`` contains the historical LLPE engine,
ported to LLVM 14.x and built as ``CanaryPE``. It provides the ``llpe-analysis``
and ``llpe`` legacy passes plus supporting infrastructure for specialization,
symbolic reasoning, and committed IR rewriting.

See :doc:`pe` for the main components and integration notes.

Software prefetching
--------------------

``lib/Optimization/Prefetch/`` implements the ``SWPrefetchingLLVMPass`` legacy
function pass for profile-guided software prefetching of indirect memory
accesses.

See :doc:`swprefetching` for its distance providers, options, and workflow.

Integration pattern
-------------------

Typical consumers of this subtree use one of these patterns:

1. Run ``llvm_utils::optimiseModule`` to apply a standard LLVM cleanup pipeline.
2. Add selected scalar or interprocedural legacy passes to a pass manager.
3. Run Lotus analyses, verification, or checker pipelines on the optimized IR.

.. toctree::
   :maxdepth: 2

   scalar
   ip
   pe
   swprefetching
   pass_ordering
