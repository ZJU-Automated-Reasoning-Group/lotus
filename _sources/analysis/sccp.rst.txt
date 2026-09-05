SCCP — Sparse Conditional Constant Propagation
===============================================

Sparse Conditional Constant Propagation (SCCP) is a lattice-based dataflow
analysis that propagates constant values through a function while simultaneously
identifying unreachable (dead) code regions.

**Headers**: ``include/Analysis/SCCP/``

**Implementation**: ``lib/Analysis/SCCP/``

**Public API**:
- ``lotus::analysis::sccp::runSCCPOnFunction`` — analyze a single function
- ``lotus::analysis::sccp::runSCCPOnModule`` — analyze all functions in a module

Overview
--------

SCCP tracks each ``llvm::Value`` through a three-valued lattice:

- **Top** — the value is not yet known (the initial state)
- **Constant** — the value is known to be a specific ``ConstantInt``
- **Bottom** — the value is not a constant (overdefined)

The solver iterates using a dual worklist: a CFG worklist propagates
executability through basic blocks (tracking reachable blocks and edges),
and an SSA worklist propagates value updates through the use-def chain.

When a branch or switch condition is resolved to a constant, only the
corresponding outgoing edge is marked executable, causing the solver to
discover dead blocks that become unreachable under any feasible input.

Supported Instructions
----------------------

The solver evaluates the following instruction categories:

- **PHINode** — merges values from executable incoming edges
- **BinaryOperator** — Add, Sub, Mul, And, Or, Xor, SDiv, SRem
- **ICmpInst** — EQ, NE, SLT, SLE, SGT, SGE
- **CastInst** — ZExt, SExt, Trunc, BitCast
- **BranchInst** — conditional branches resolved when condition is constant
- **SwitchInst** — case selection resolved when discriminator is constant
- **LoadInst** — loads from read-only global variables with known initializers
- **CallBase** — calls to non-constant functions produce Bottom
- **AllocaInst / GetElementPtrInst** — produce Bottom (non-constant)

Read-Only Globals
-----------------

Before analysis, SCCP scans the module for global variables that are never
stored to and have a constant initializer. These read-only globals are
treated as constants when encountered in load instructions, enabling
constant propagation across global reads.

Results
-------

The analysis returns a ``FunctionResult`` containing:

- **constants** — a map from ``llvm::Value *`` to ``ConstantInt *`` for
  values that were resolved to constants
- **dead_blocks** — the set of basic blocks found to be unreachable

The module-level entry point (``runSCCPOnModule``) aggregates results across
all functions and also provides cross-function constant discovery.

Example
-------

.. code-block:: cpp

   #include "Analysis/SCCP/SCCP.h"

   llvm::Module &M = ...;
   auto result = lotus::analysis::sccp::runSCCPOnModule(M);

   for (const auto &[value, constant] : result.constants) {
     value->replaceAllUsesWith(constant);
   }
   for (llvm::BasicBlock *dead : result.dead_blocks) {
     dead->eraseFromParent();
   }
