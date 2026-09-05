AFLGo Compiler Plugin
=====================

``include/Fuzzing/AFLGoCompiler/`` and ``lib/Fuzzing/AFLGoCompiler/`` implement
the compile-time LLVM plugin used to inject directed-fuzzing targets.

**Location**: ``include/Fuzzing/AFLGoCompiler/``,
``lib/Fuzzing/AFLGoCompiler/``

**Main components**:

- ``AFLGoTargetInjectionPass`` inserts target metadata during compilation.
- ``Plugin.cpp`` registers the pass as an LLVM plugin.

This layer prepares IR so later link-time instrumentation can compute and emit
distance-guided feedback.

Place in the pipeline
---------------------

Run this plugin while compiling the target program so target metadata survives
into the link-time module.  The plugin identifies and marks targets; it does
not calculate the final distance feedback by itself.  That later work belongs
to the analysis and linker stages, so use compatible target descriptions across
all three stages.

See also :doc:`fuzzing_analysis` and :doc:`aflgo_linker`.
