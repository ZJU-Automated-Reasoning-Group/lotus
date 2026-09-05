Verification Transformation Passes
==================================

``include/Verification/Transform/`` and ``lib/Verification/Transform/`` provide
IR rewrites that prepare programs for model checking and abstract
interpretation.

**Representative passes**:

- ``MakeNondetPass`` and ``DeleteUndefinedPass`` normalize undefined behavior.
- ``InstrumentAllocPass`` and ``PrepareOverflowsPass`` expose verification
  checks explicitly.
- ``InitializeUninitializedPass`` and ``BreakInfiniteLoopsPass`` simplify the
  state space.
- loop and API rewriting passes adapt LLVM IR to backend expectations.

These transforms are typically used before handing a module to verifier tools.

Pipeline guidance
-----------------

Choose only the transformations required by the target backend and property.
Each rewrite changes the IR presented to the verifier, so retain source and
debug information where diagnostics depend on it, and inspect the transformed
module when introducing a new pass sequence.  These passes prepare a problem;
they do not replace the backend's property checking.

See also :doc:`../user_guide/instrumentation_passes` and :doc:`backend`.
