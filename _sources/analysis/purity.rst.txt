Purity Analysis
===============

Function-level purity and side-effect analysis for LLVM bitcode.

**Headers**: ``include/Analysis/Purity/``

**Implementation**: ``lib/Analysis/Purity/``

**Build target**: ``CanaryPurityAnalysis``

Overview
--------

The Purity Analysis subsystem determines whether functions have side effects
(mutate memory, perform I/O, etc.) and classifies them into purity levels:
*const*, *pure*, *impure*, or *unknown*. This information is used by
optimization passes, checkers, and alias analyses to reason about call-site
effects.

Purity classification follows the standard convention:

- **Const**: The function reads no reachable memory and has no observable side
  effects. Its return value depends only on its arguments.
- **Pure**: The function may read reachable memory but does not write to it and
  has no observable side effects.
- **Impure**: The function writes to reachable memory or has observable side
  effects.
- **Unknown**: No classification could be determined.

Components
----------

FunctionPurityAnalysis
~~~~~~~~~~~~~~~~~~~~~~

**File**: ``FunctionPurityAnalysis.h``

The top-level analysis driver. Runs intraprocedural purity inference across
a module, using optional MemorySSA-backed summaries for more precise results.

.. code-block:: cpp

   #include "Analysis/Purity/FunctionPurityAnalysis.h"

   lotus::analysis::purity::FunctionPurityAnalysis purity(M);
   purity.run();

   if (purity.isConst(F)) { /* ... */ }
   if (purity.isPure(F))  { /* ... */ }
   if (purity.isKnown(F)) { /* ... */ }

   PurityKind k = purity.getPurity(F);
   FunctionEffectSummary fx = purity.getEffects(F);

**Configuration** (``FunctionPurityAnalysisOptions``):

- ``MemorySSAMode``: controls whether MemorySSA-based summaries are used
  (``Disabled`` or ``UseIfAvailable``)
- ``declarationSummaryProviders``: external providers for declared (no-body)
  function summaries
- ``externalSummaryProviders``: external providers for summary files

PurityKind
~~~~~~~~~~

.. code-block:: cpp

   enum class PurityKind {
     Const = 0,   // No memory reads/writes, no side effects
     Pure = 1,    // May read memory, no writes or side effects
     Impure = 2,  // May write memory or have side effects
     Unknown = 3  // Classification not determined
   };

FunctionEffectSummary
~~~~~~~~~~~~~~~~~~~~~

A structured summary of a function's observable effects:

- ``readsReachableMemory`` — The function may read globally reachable memory.
- ``writesReachableMemory`` — The function may write to globally reachable memory.
- ``hasObservableSideEffects`` — The function may perform I/O, volatile accesses, etc.
- ``hasUnknownEffects`` — The analysis could not fully determine the effects.
- ``source`` — Where the summary came from (``SummarySource``).
- ``confidence`` — Confidence level of the summary (``SummaryConfidence``).

Purity Inference Passes
~~~~~~~~~~~~~~~~~~~~~~~

- **PurityAttrInferencePass** — Infers purity from LLVM function attributes
  (``readnone``, ``readonly``, etc.).
- **PurityUnknownImpactPass** — Identifies functions whose unknown effects
  may impact downstream analyses.
- **MemorySSAPuritySummary** — Builds purity summaries from MemorySSA analysis
  for more precise per-call-site reasoning.

External Summary Store
~~~~~~~~~~~~~~~~~~~~~~

**File**: ``ExternalPuritySummaryStore.h``

Loads and caches external purity summaries for third-party or unavailable
functions, enabling interprocedural purity analysis across library boundaries.

Usage
-----

.. code-block:: cpp

   #include "Analysis/Purity/FunctionPurityAnalysis.h"

   llvm::Module &M = ...;
   lotus::analysis::purity::FunctionPurityAnalysis purity(M);
   purity.run();

   for (auto &F : M) {
     if (purity.isConst(&F))
       outs() << F.getName() << " is const\n";
     else if (purity.isPure(&F))
       outs() << F.getName() << " is pure\n";
     else if (purity.isAtMostPure(&F))
       outs() << F.getName() << " is at most pure\n";
   }

See Also
--------

- :doc:`../transform/index` — Optimisation passes that consume purity info
- :doc:`../ir/shadowmemssa` — ShadowMem SSA used for summary generation
