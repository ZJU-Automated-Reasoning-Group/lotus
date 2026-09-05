PE
==

``lib/Optimization/PartialEvaluation/`` contains the LLPE partial-evaluation
infrastructure.

**Headers**: ``include/Optimization/PartialEvaluation/``

**Implementation**: ``lib/Optimization/PartialEvaluation/``

Overview
--------

This subsystem contains the historical LLPE code used for aggressive
specialization, symbolic execution style reasoning, and partial evaluation over
LLVM IR. It is built as the ``CanaryPE`` static library and is separate from
the scalar, IPO, and prefetch libraries. It is not currently surfaced through
the ``lotus-opt-ipo`` or ``lotus-opt-prefetch`` frontends.

Main components
---------------

- ``LLPE.h`` defines the main analysis and integration machinery.
- ``ShadowInlines.h`` models shadow values, instructions, stores, and
  specialized state used during specialization.
- ``SharedTree.h`` provides persistent tree structures used to represent shared
  analysis state.
- ``LLPECopyPaste.h`` contains cloning and IR-rewriting helpers.
- ``TopLevel.cpp`` registers the ``llpe-analysis`` legacy ``ModulePass``.
- ``Integrator.cpp`` registers the ``llpe`` legacy ``ModulePass`` that commits
  the specialization chosen by the analysis.

Passes and options
------------------

- ``llpe-analysis`` performs the analysis phase and constructs specialization
  contexts.
- ``llpe`` runs the integrator and commits the rewritten IR.
- ``-llpe-root=<name>`` selects the root function to specialize; the default is
  ``main``.

The subsystem also exposes a large set of LLPE-specific debugging and tuning
flags in ``CommandLine.cpp`` for controlling graph dumps, specialization limits,
environment modeling, statistics, and optional transforms.

Notes
-----

The PE subsystem is primarily infrastructure code. If you are looking for
front-end optimization passes exposed as stable tools, start with
:doc:`index` and :doc:`../tools/optimization/index`.
