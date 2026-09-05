ModRef Function Effect Specifications
====================================

The ``include/Annotation/ModRef/`` headers define the external mod/ref summary
format used to model library functions for alias and checker pipelines.

**Location**: ``include/Annotation/ModRef/``

**Main APIs**:

- ``ModRefEffect`` and ``ModRefEffectSummary`` describe a function's memory
  reads and writes.
- ``ExternalModRefTable`` loads and stores per-function summaries.
- ``ExternalModRefTablePrinter`` emits human-readable tables for debugging.

**What it models**:

- Direct-memory versus reachable-memory effects.
- Argument and return positions through ``APosition``.
- External summaries for functions whose bodies are unavailable.

**Used by**:

- Alias analyses that need library-side memory effects.
- Bug-finding and verification passes that need consistent side-effect models.

Modeling guidance
-----------------

Use a direct-memory effect for the position itself and a reachable-memory
effect when the function accesses memory through that position.  Summaries are
particularly important for declarations without an LLVM body: without one, a
client must choose between an imprecise conservative model and skipping an
effect it cannot justify.  Keep shared library models in this format so alias,
checker, and verification clients agree on call behavior.

See also :doc:`annotation`, :doc:`../alias/spec`, and :doc:`../alias/alias_analysis`.
