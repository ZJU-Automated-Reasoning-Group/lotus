Nisse
=====

``Nisse`` contains an LLVM new-pass-manager analysis and transform under
``Transform/Nisse``.

**Headers**: ``include/Transform/Nisse/``

**Implementation**: ``lib/Transform/Nisse/``

Overview
--------

The subsystem packages graph-based CFG edge analyses together with several
instrumentation and rewrite passes. It now includes the original ``Nisse`` and
``KS`` pipelines plus the newer PRUE flow, which inserts delayed delta-counter
updates and rewrites them after optimization.

Main components
---------------

- ``Edge.cpp`` implements CFG edge instrumentation, including SESE-aware and
  local delta-counter updates.
- ``NisseAnalysis.cpp`` provides ``NisseAnalysis`` and ``KSAnalysis`` to build
  edge sets, spanning trees, and well-founded loop annotations.
- ``NissePass.cpp`` implements ``NissePass`` and ``KSPass``, which materialize
  global counter/index arrays, emit ``info.prof``, and call the profiling
  runtime in ``prof.c``.
- ``PRUE.cpp`` implements ``DeltaCounterPass`` and ``PruePass`` for the
  optimizer-friendly PRUE instrumentation/rewrite pipeline.
- ``NissePlugin.cpp`` registers the analyses and the pass pipeline names for
  the new pass manager plugin.
- ``NissePropagation.cpp`` is a standalone propagation utility that reconstructs
  full edge and basic-block frequencies from ``.graph`` and profiling files.

Registered passes
-----------------

The plugin registers the following module pipeline names:

- ``nisse`` - run the main Nisse edge instrumentation pass
- ``ks`` - run the KS variant after loop simplification and critical-edge
  breaking
- ``prue-delta`` - insert local delta counters and mark updates for PRUE
- ``prue`` - perform the late PRUE rewrite

In addition, the plugin hooks ``DeltaCounterPass`` at pipeline start and
``PruePass`` at the optimizer's last extension point.

Artifacts
---------

- ``info.prof`` records each instrumented function and the number of counters
  assigned to it.
- ``main.prof`` is emitted by ``nisse_pass_print_data`` in ``prof.c`` and stores
  counter values indexed by edge id.
- ``<function>.graph`` is written by the analysis to describe the CFG, spanning
  tree, and instrumented edges.
- ``NissePropagation.cpp`` can consume those files and emit propagated
  ``.edges`` and ``.bb`` summaries.

Notes
-----

``CanaryNisse`` is built from ``Edge.cpp``, ``NisseAnalysis.cpp``,
``NissePass.cpp``, ``PRUE.cpp``, ``NissePlugin.cpp``, and ``prof.c`` as defined
in ``lib/Transform/Nisse/CMakeLists.txt``. ``NissePropagation.cpp`` currently
serves as a standalone utility rather than part of the library target.
