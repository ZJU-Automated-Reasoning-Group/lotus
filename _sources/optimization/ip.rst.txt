Interprocedural MemorySSA Optimizations
=======================================

``lib/Optimization/IPO/`` contains the interprocedural ShadowMem and MemorySSA
optimizations used by the optimization tooling.

**Location**: ``lib/Optimization/IPO/``

**Library target**: ``CanaryOptimizationIPO``

Prerequisites
-------------

These passes expect IR that has already been prepared with SeaDsa-based
ShadowMem instrumentation. The ``lotus-opt-ipo`` frontend inserts the required
setup passes before running any IPO optimization:

- ``RemovePtrToInt``
- ``AllocWrapInfo``
- ``DsaLibFuncInfo``
- ``AllocSiteInfo``
- ``DsaAnalysis``
- ``ShadowMem``

Without that instrumentation, the IPO passes have no ``shadow.mem.*`` def-use
chains to analyze.

Main passes
-----------

- ``IPDeadStoreElimination`` (pass name ``ipdse``) removes stores and some
  global initializers whose ShadowMem def-use chains never reach a
  ``shadow.mem.load``. It traverses phis plus call/return edges encoded by
  ``shadow.mem.arg.*`` and ``shadow.mem.in/out``.
- ``IPRedundantLoadElimination`` (pass name ``ip-rle``) removes repeated loads
  inside a basic block when both the pointer operand and the MemorySSA TLVar are
  identical and no intervening non-ShadowMem memory write occurs.
- ``IPStoreSinking`` (pass name ``ip-sink``) moves a store and its adjacent
  ``shadow.mem.store`` marker forward within a basic block to just before the
  earliest observable user when all crossed instructions are side-effect free.
- ``IPStoreToLoadForwarding`` (pass name ``ip-forward``) walks MemorySSA edges
  across phis and direct-call boundaries to replace a load with a unique
  reaching stored value.

Configuration
-------------

All four passes default to singleton-only regions and expose hidden tuning
flags:

- ``IPDeadStoreElimination``: ``-ipdse-only-singleton``,
  ``-ipdse-max-def-use``
- ``IPRedundantLoadElimination``: ``-ip-rle-only-singleton``
- ``IPStoreSinking``: ``-ip-sink-only-singleton``
- ``IPStoreToLoadForwarding``: ``-ip-forward-only-singleton``

These passes are exposed through the interprocedural optimization front-end.

See also :doc:`index` and :doc:`../tools/optimization/interprocedural`.
