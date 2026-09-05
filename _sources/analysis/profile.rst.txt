Profile Analysis
================

Hot-code detection based on profiling data for feedback-directed optimization.

**Headers**: ``include/Analysis/Profile/``

**Implementation**: ``lib/Analysis/Profile/``

**Build target**: ``CanaryProfile``

Overview
--------

The Profile Analysis module provides profile-guided hot-code detection using
LLVM's ``BlockFrequencyInfo`` and ``BranchProbabilityInfo``. It identifies
frequently executed code regions (hot functions, loops, basic blocks) from
profiling data, enabling downstream optimizations and analyses to focus on
performance-critical paths.

Hot Class
---------

**File**: ``Profile/Hot.h``

The central ``Hot`` class queries profiling metadata to classify code regions
by execution frequency:

.. code-block:: cpp

   #include "Analysis/Profile/Hot.h"

   lotus::analysis::profile::Hot hot(M, getBFI, getBPI);

**Query Methods**:

For instructions, basic blocks, and loops:

- ``getStaticInstructions()`` — Total static instruction count.
- ``getSelfInstructions()`` — Instructions excluding callees.
- ``getTotalInstructions()`` — Dynamic instruction count (sum over executions).
- ``getInvocations()`` — How many times the region was entered at runtime.
- ``hasBeenExecuted()`` — Whether the region was ever reached.
- ``getDynamicTotalInstructionCoverage()`` — Fraction of dynamic instructions
  this region accounts for.

Loop-specific:

- ``getIterations()`` — Average trip count per invocation.
- ``getAverageLoopIterationsPerInvocation()`` — Mean iterations per entry.
- ``getAverageTotalInstructionsPerInvocation()`` — Mean instructions per entry.
- ``getAverageTotalInstructionsPerIteration()`` — Mean instructions per trip.

Usage
-----

.. code-block:: cpp

   #include "Analysis/Profile/Hot.h"

   llvm::Module &M = ...;
   auto getBFI = [&](llvm::Function &F) -> llvm::BlockFrequencyInfo & { ... };
   auto getBPI = [&](llvm::Function &F) -> llvm::BranchProbabilityInfo & { ... };

   lotus::analysis::profile::Hot hot(M, getBFI, getBPI);

   for (auto &F : M) {
     for (auto &B : F) {
       for (auto &I : B) {
         if (hot.hasBeenExecuted(&I)) {
           uint64_t dyn = hot.getTotalInstructions(&I);
           // Focus optimisation effort on high-dynamic-count instructions
         }
       }
     }
   }

See Also
--------

- :doc:`../optimization/swprefetching` — Profile-guided software prefetching
- :doc:`../optimization/index` — Optimisation passes consuming profile data
