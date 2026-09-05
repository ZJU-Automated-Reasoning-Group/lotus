AFLGo Link-Time Instrumentation
===============================

``include/Fuzzing/AFLGoLinker/`` and ``lib/Fuzzing/AFLGoLinker/`` implement the
link-time instrumentation passes for AFLGo-style directed fuzzing.

**Location**: ``include/Fuzzing/AFLGoLinker/``, ``lib/Fuzzing/AFLGoLinker/``

**Main passes**:

- ``AFLGoDistanceInstrumentationPass`` instruments distance feedback.
- ``FunctionDistancePass`` injects function-level distance data.
- ``DAFLInstrumentationPass`` adds DAFL-specific instrumentation.
- ``DuplicateTargetRemovalPass`` and ``AFLGoTargetInjectionFixupPass`` clean up
  target metadata before final code generation.

**Role in the pipeline**:

- consume the analyses from :doc:`fuzzing_analysis`
- insert profile-guiding feedback at LTO time
- finalize target handling for directed greybox fuzzers

Operational notes
-----------------

Use the linker passes with the distance and target facts generated for the same
program revision.  The cleanup passes run before final code generation so
instrumentation observes a consistent set of target annotations.  Validate the
resulting bitcode or executable with a small corpus before interpreting
distance feedback as a fuzzing-quality signal.

See also :doc:`fuzzing_support`.
