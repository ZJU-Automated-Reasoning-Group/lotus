LLVM Utility Layer
==================

``include/Utils/LLVM/`` and ``lib/Utils/LLVM/`` provide the common LLVM-facing
support code reused by front-ends and analysis passes.

**Main components**:

- ``IO/ReadIR`` and ``IO/WriteIR`` for reading and writing LLVM modules.
- ``InstructionUtils`` and ``Demangle`` for IR inspection.
- ``GraphWriter`` and ``LLVMBgl`` for graph traversal and visualization.
- ``Statistics``, ``RecursiveTimer``, and ``Log`` for diagnostics.

This layer is the shared glue between Lotus analyses and LLVM infrastructure.

Using the layer
---------------

Tools should use the module I/O helpers instead of open-coding bitcode loading
and writing, so diagnostics and command-line behavior remain consistent.
Analysis code can use the instruction and demangling helpers when it needs to
recognize IR patterns without duplicating LLVM-specific boilerplate.  Keep
LLVM-version compatibility work in this layer where it can be shared.

See also :doc:`utilities`.
