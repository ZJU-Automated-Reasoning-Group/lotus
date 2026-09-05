Multiplicity — Allocation Multiplicity Classification
======================================================

Allocation multiplicity classifies each allocation site (global, stack, or
heap) as either *Unique* (executed at most once per program invocation) or
*Summary* (potentially executed multiple times).

**Headers**: ``include/Analysis/Multiplicity/``

**Implementation**: ``lib/Analysis/Multiplicity/``

**Public API**:
- ``lotus::analysis::multiplicity::classifyModuleMultiplicity``
  — classify all allocations in a module

Overview
--------

The analysis inspects every allocation site in a module and assigns a
multiplicity classification:

- **Unique** — the allocation executes at most once per program run.
  Applies to:
  - All global variables (always Unique)
  - Stack allocations (alloca) in functions without loops
  - Heap allocations (malloc/calloc/realloc) in functions that are
    both loop-free and called from at most one call site
- **Summary** — the allocation may execute multiple times.
  Applies to:
  - Allocations inside loop-bearing functions
  - Heap allocations in functions called from multiple call sites

Heap Allocation Detection
-------------------------

Heap allocation functions are identified using the ``AliasSpecManager``,
which recognizes standard allocators (``malloc``, ``calloc``, ``realloc``,
``operator new``, etc.) by their known function roles.

Usage
-----

.. code-block:: cpp

   #include "Analysis/Multiplicity/MultiplicityClassifier.h"

   llvm::Module &M = ...;
   auto result = lotus::analysis::multiplicity::
       classifyModuleMultiplicity(M);

   for (const auto &[value, mult] : result.allocations) {
     if (mult == lotus::analysis::multiplicity::AllocationMultiplicity::Unique)
       errs() << "Unique allocation: " << *value << "\n";
   }

Conservative interpretation
---------------------------

``Summary`` means the analysis cannot establish that an allocation executes
only once; it is not evidence that the allocation necessarily repeats at
runtime.  In particular, loops, multiple call sites, indirect calls, and
incomplete external-call modeling should be treated conservatively by clients
that use multiplicity to select heap abstractions or optimization strategies.
