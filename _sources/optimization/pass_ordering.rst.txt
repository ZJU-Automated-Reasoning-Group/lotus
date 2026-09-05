Pass Ordering Utilities
=======================

Utility for running LLVM's standard optimization pass pipelines (O0–O3)
programmatically.

**Headers**: ``include/Optimization/PassOrdering/``

**Implementation**: ``lib/Optimization/PassOrdering/``

**Build target**: ``CanaryOptimizationPipeline``

Overview
--------

The PassOrdering module provides a small API for running LLVM's default
optimization pass pipelines (O0, O1, O2, O3) on a module without going through
the ``opt`` tool. This is useful when a Lotus tool wants to perform standard
LLVM cleanup or optimisation as a preprocessing step before running its own
analyses.

ModuleOptimizer
---------------

**File**: ``ModuleOptimizer.h``

.. code-block:: cpp

   #include <Optimization/PassOrdering/ModuleOptimizer.h>

   llvm::Module *M = ...;
   llvm_utils::optimiseModule(M, llvm::OptimizationLevel::O2);

The function ``llvm_utils::optimiseModule`` composes the default per-module
pipeline for the requested ``llvm::OptimizationLevel`` using
``llvm::PassBuilder``.

Use cases
---------

- **Preprocessing**: Apply O1/O2 cleanup before alias analysis or bug checking
  to remove dead code, inline trivial functions, and simplify the IR.
- **Testing**: Quickly run a standard pipeline to verify optimisation
  interaction with Lotus passes.
- **Custom tooling**: Any tool that needs a standard optimisation baseline
  without invoking ``opt`` as a separate process.

.. code-block:: cpp

   #include <Optimization/PassOrdering/ModuleOptimizer.h>

   // Apply O2-level optimisations as preprocessing
   llvm_utils::optimiseModule(M, llvm::OptimizationLevel::O2);

   // Then run Lotus analysis on the optimised IR
   lotus::analysis::nullpointer::NullCheckAnalysis nullCheck(M);
   nullCheck.run();

See Also
--------

- :doc:`../optimization/scalar` — Individual scalar optimisation passes
- :doc:`../optimization/ip` — Interprocedural optimisation passes
