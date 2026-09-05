Scalar Optimizations
====================

``lib/Optimization/Scalar/`` contains scalar optimization passes packaged in
the ``CanaryOptimizationScalar`` static library.

**Headers**: ``include/Optimization/Scalar/``

**Implementation**: ``lib/Optimization/Scalar/``

Available passes
----------------

- ``AggressiveInliner`` is a legacy ``ModulePass`` registered as ``ainline``.
- ``DSE`` is Lotus' LLVM-14-derived dead-store elimination implementation.
- ``GVN`` is Lotus' LLVM-14-derived global value numbering pass.
- ``LICM`` is Lotus' LLVM-14-derived loop-invariant code motion pass.

AggressiveInliner
-----------------

**Files**: ``include/Optimization/Scalar/AggressiveInliner.h``,
``lib/Optimization/Scalar/AggressiveInliner.cpp``

This pass walks every function in the module and tries to inline each direct
call site unless the callee name appears in ``-ainline-noinline``.

- **Registration**: ``ainline``
- **Factory**: ``llvm::createAggressiveInlinerPass()``
- **Useful option**: ``-ainline-noinline=foo,bar``

Use it when analysis precision benefits from collapsing call boundaries and the
resulting code-size increase is acceptable.

Dead Store Elimination
----------------------

**Files**: ``include/Optimization/Scalar/DeadStoreElimination.h``,
``lib/Optimization/Scalar/DeadStoreElimination.cpp``

Lotus ships LLVM's dead-store elimination implementation for LLVM 14. The
legacy pass is registered as ``dse``, and the header also exposes the new pass
manager interface ``llvm::DSEPass``.

- **Legacy factory**: ``llvm::createDeadStoreEliminationPass()``
- **Dependencies**: alias analysis, dominator tree, post-dominator tree,
  MemorySSA, memory dependence, target library info, loop info

Use it to remove stores that are overwritten or otherwise proven dead before a
read.

GVN
---

**Files**: ``include/Optimization/Scalar/GVN.h``,
``lib/Optimization/Scalar/GVN.cpp``

This is Lotus' copy of LLVM 14 global value numbering. It eliminates fully
redundant expressions, performs dead-load elimination, and optionally enables
partial redundancy elimination.

- **Legacy registration**: ``gvn``
- **Legacy factory**: ``llvm::createGVNPass(bool NoMemDepAnalysis = false)``
- **New PM entry**: ``llvm::GVNPass``
- **Tunable options**: ``-enable-pre``, ``-enable-load-pre``,
  ``-enable-load-in-loop-pre``, ``-enable-gvn-memdep``

Use it when common-subexpression elimination and load simplification are useful
before analysis or code generation.

LICM
----

**Files**: ``include/Optimization/Scalar/LICM.h``,
``lib/Optimization/Scalar/LICM.cpp``

Lotus ships LLVM 14 LICM for hoisting and sinking loop-invariant code and for
promoting must-alias loop memory to SSA values when safe.

- **Legacy registration**: ``licm``
- **Legacy factories**: ``llvm::createLICMPass()`` and the parameterized
  ``llvm::createLICMPass(unsigned, unsigned, bool)``
- **New PM entries**: ``llvm::LICMPass`` and ``llvm::LNICMPass``
- **Relevant options**: ``-disable-licm-promotion``,
  ``-licm-control-flow-hoisting``, ``-licm-mssa-optimization-cap``,
  ``-licm-mssa-no-acc-for-promotion-cap``

LICM is especially useful when evaluating alias-analysis precision because its
ability to hoist and promote memory operations depends directly on alias and
MemorySSA information.

Notes
-----

- ``AggressiveInliner`` is Lotus-specific and intentionally simple.
- ``DSE``, ``GVN``, and ``LICM`` are derived from LLVM 14 and kept in-tree so
  Lotus tools can depend on stable pass behavior across the framework.
