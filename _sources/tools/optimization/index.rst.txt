Optimization Tools
==================

This section documents the optimization-related command-line tools under
``tools/optimization/``.

The current front ends cover two focused parts of ``lib/Optimization/``:

- ``lotus-opt-ipo`` drives the passes in ``lib/Optimization/IPO/`` via
  ``tools/optimization/lotus-opt-ipo.cpp``
- ``lotus-opt-prefetch`` drives the software prefetching implementation in
  ``lib/Optimization/Prefetch/`` via
  ``tools/optimization/lotus-opt-prefetch.cpp``

Other optimization libraries, such as ``Scalar/``, ``Pipeline/``, and
``PartialEvaluation/``, exist in the source tree but are not documented here as
standalone tools because this directory does not currently expose separate
front-end binaries for them.

**Location**: ``tools/optimization/``
**Additional frontends**: the currently disabled
``tools/optimization/lotus-opt-lif.cpp``

lotus-opt-purity — Purity Inference Driver
-------------------------------------------

Purity inference tool that classifies LLVM functions into one of four purity
levels using a MemorySSA-based pipeline (SeaDsa/ShadowMem):

- **Const** (``readnone``) — function does not access memory at all
- **Pure** (``readonly``) — function only reads memory, no writes
- **Impure** — function writes to memory
- **Unknown** — could not be determined

**Source**: ``tools/optimization/lotus-opt-purity.cpp``. See
``lib/Analysis/Purity/`` for the underlying analysis framework.

Key options:

``-o <file>``
  Output bitcode with optional applied attributes.

``-S``
  Emit assembly instead of bitcode.

``--purity-summary-file <file>``
  External JSON summary store (load + save back).

``--report-json <file>``
  Write purity report as JSON.

``--apply-attrs``
  Materialize ``readnone``/``readonly`` LLVM attributes on function
  declarations.

``--include-suggested``
  Use suggested (non-validated) external summaries.

``--invalidate-summary <name>``
  Reject a summary and recompute dependents.

``--disable-memoryssa-prep``
  Skip the SeaDsa/ShadowMem pipeline.

``--purity-log``
  Verbose diagnostics to stderr.

``--sea-dsa=<mode>``
  SeaDsa mode (defaults to ``ci`` for purity analysis).

Usage examples:

.. code-block:: bash

   # Basic purity analysis, output bitcode with attributes
   lotus-opt-purity input.bc -o output.bc --apply-attrs

   # Generate JSON report
   lotus-opt-purity input.ll -S --report-json purity.json

   # Use external summary store with validation
   lotus-opt-purity input.bc -o output.bc \\
     --purity-summary-file summaries.json --apply-attrs

.. toctree::
   :maxdepth: 1

   interprocedural
   prefetch
