LIF
===

Isochronous transformation and taint analysis for side-channel mitigation.

**Headers**: ``include/Security/LIF``

**Implementation**: ``lib/Security/LIF``

**Build target**: ``CanaryLIF``

Overview
--------

The ``LIF`` subsystem combines a taint analysis with IR-to-IR transformations
that rewrite code into a more isochronous form. Its goal is mitigation rather
than bug finding: instead of only reporting secret-dependent control flow, it
can transform functions so they execute a uniform set of instructions
regardless of sensitive inputs.

The implementation lives under the unified ``lib/Security`` tree alongside the
``ConstantTime`` and ``Spectre`` components.

Main Components
---------------

TaintAnalysis
~~~~~~~~~~~~~

**Files**: ``Taint.h``, ``Taint.cpp``

``lotus::lif::analysis::TaintAnalysis`` traverses the module call graph and
marks values that depend on secret data.

**Key properties**:

- secrets are discovered from ``annotate("secret")`` metadata
- taint propagates through both data and selected control dependencies
- the analysis is aware of later linearization, so it does not blindly taint
  every control-dependent value

IsochronousPass
~~~~~~~~~~~~~~~

**Files**: ``Isochronous.h``, ``Isochronous.cpp``

``lotus::lif::transform::IsochronousPass`` rewrites functions into an
isochronous form.

An isochronous function is intended to execute the same set of instructions
regardless of inputs, which is useful for mitigating side-channel leakage.

Control-Flow Representation
~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Files**:

- ``CCFG.h``, ``CCFG.cpp``
- ``Loop.h``, ``Loop.cpp``
- ``Cond.h``, ``Cond.cpp``
- ``Func.h``, ``Func.cpp``

These helpers support the transformation:

- ``CCFG`` builds a collapsed control-flow graph with loops summarized into
  acyclic nodes
- ``LoopWrapper`` records loop headers, latches, exits, and synthetic phi
  nodes needed for transformation
- condition binding computes incoming and outgoing predicates for basic blocks
- function rewriting performs partial linearization and instruction rewriting

Transformation Strategy
-----------------------

The implementation follows a control-flow linearization approach inspired by
partial linearization work such as Moll and Hack's PLDI paper.

At a high level, the pass:

1. runs taint analysis to find secret-dependent values
2. prepares loop and control-flow metadata
3. computes block and edge conditions
4. partially linearizes secret-dependent control flow
5. rewrites loads, stores, phi nodes, and selected interfaces so execution is
   more uniform

Public API
----------

Important types and entry points include:

- ``lotus::lif::analysis::TaintAnalysis``
- ``lotus::lif::analysis::TaintedInfo``
- ``lotus::lif::transform::IsochronousPass``
- ``lotus::lif::transform::FuncWrapper``
- ``lotus::lif::transform::LoopWrapper``

Usage Notes
-----------

The transformation expects LLVM IR with enough structure for loop and CFG
reasoning. In particular, the isochronous pass assumes functions have unique
exit points.

Typical use cases:

- mitigating secret-dependent branches in sensitive code
- experimenting with constant-time or uniform-execution rewrites
- building research prototypes for side-channel-resistant transformations

Related Components
------------------

- :doc:`crypto` covers ``lib/Security/ConstantTime`` for CT-LLVM-style
  constant-time checking.
- :doc:`spectre` covers ``lib/Security/Spectre`` for speculative cache
  vulnerability analysis.
