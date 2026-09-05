ParameterSummary — Parameter Effect Summary Analysis
=====================================================

Parameter effect summaries capture per-function side effects on function
parameters, enabling downstream analyses to reason about interprocedural
effects without re-analyzing callees.

**Headers**: ``include/Analysis/ParameterSummary/``

**Implementation**: ``lib/Analysis/ParameterSummary/``

**Public API**:
- ``lotus::analysis::parametersummary::computeParameterEffectSummaries``
  — compute summaries for all functions in a module

Overview
--------

For each function in a module, the analysis produces a
``ParameterEffectSummary`` that records three kinds of side-effect
information:

- **paramFreed** — whether a parameter is freed (or released / unlocked)
  inside the function or its callees
- **paramDereferenced** — whether a parameter's pointee is loaded from
  or stored to
- **returnIsAllocated** — whether the return value is a freshly allocated
  object (from an allocator function)

Resource Table
--------------

The analysis uses a ``ResourceTable`` that maps known function names to
their resource-management roles:

.. code-block:: text

   Allocator       — malloc, calloc, realloc, operator new, ...
   Deallocator     — free, operator delete, ...
   Acquire/Release — pthread_mutex_lock / pthread_mutex_unlock, ...
   Dereference     — memcpy, memset, strlen, ...

The table is populated from a built-in database of standard library and
POSIX functions, and can be extended via module-level annotation specs.

Transitive Composition
----------------------

When the call graph has a topological ordering (no recursion), summaries are
composed transitively: if a function calls a callee that frees one of its
parameters, the caller's corresponding argument is also marked as freed.
This transitive propagation continues through the call chain.

Usage
-----

.. code-block:: cpp

   #include "Analysis/ParameterSummary/ParameterEffectSummary.h"

   llvm::Module &M = ...;
   auto summaries = lotus::analysis::parametersummary::
       computeParameterEffectSummaries(M);

   for (const auto &[func, summary] : summaries) {
     if (summary.returnIsAllocated)
       errs() << func->getName() << " returns allocated memory\n";
   }
