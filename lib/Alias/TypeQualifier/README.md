# TypeQualifier

`TypeQualifier` is an opt-in, interprocedural uninitialized-data checker for
LLVM IR. It is inspired by flow-sensitive qualifier work, but this subtree is
implemented as a specialized practical checker rather than as a faithful,
generic implementation of Foster, Terauchi, and Aiken's PLDI 2002 framework.

Current scope:

- Tracks `Initialized`, `Uninitialized`, and `Unknown` qualifier states
- Computes requiredness in a separate backward pass before forward qualifier
  propagation
- Uses the existing points-to and alias facts built in this subtree
- Applies table-driven function models for allocators, init/copy helpers, and
  debug-only intrinsics
- Is disabled by default and enabled with `-DENABLE_TYPE_QUALIFIER=ON`
