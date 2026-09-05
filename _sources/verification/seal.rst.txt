Seal — Symbolic Automata for Stateful Systems
==============================================

Seal is a symbolic automata lifter for stateful software systems, published at
**CAV 2026**. It builds finite-state-machine models from LLVM bitcode by
combining loop summary analysis, symbolic execution, and lightweight abstract
interpretation.

**Location**: ``third-party/seal/`` (vendored, opt-in)

**Build toggle**: ``-DLOTUS_ENABLE_SEAL=ON``

**Publication**: *Sound and Precise Symbolic Automata Model for Stateful
Software Systems*. Xinlong Wu, Ruiyu Zhou, Peisen Yao, and Qingkai Shi.
CAV 2026.

Overview
--------

Seal lifts LLVM IR into a symbolic automaton representation that captures
a program's stateful behavior. The lifted model can be used for verification,
property checking, and behavioral analysis of stateful systems (device drivers,
protocol implementations, etc.).

The pipeline is:

1. **Loop summary analysis** — Identifies loop structure and summarizes
   iteration-space behavior.
2. **Symbolic execution** — Explores paths through the program to discover
   state transitions.
3. **Slice graph construction** — Builds a compact symbolic representation of
   control-flow slices.
4. **Finite-state-machine lifting** — Produces a symbolic automaton model with
   guard conditions on transitions.

Directory Structure
-------------------

::

   third-party/seal/
   ├── include/
   │   ├── Core/           # Execution engine, state machine, slicing
   │   ├── Memory/         # Abstract memory model (heap, stack, globals)
   │   ├── Support/        # Z3 integration, ADTs, instruction visitors
   │   └── Transform/      # LLVM-to-IR lowering passes
   ├── lib/
   │   ├── Core/           # Executor, loop analysis, state management
   │   ├── Memory/         # Memory block implementations
   │   ├── Support/        # Z3 helpers, DL, timing
   │   └── Transform/      # LLVM IR canonicalization passes
   └── tools/
       └── seal/           # Main seal binary + FSM lifting pass

Build
-----

.. code-block:: bash

   cmake -S . -B build -DLOTUS_ENABLE_SEAL=ON
   cmake --build build -j$(nproc)

Usage
-----

.. code-block:: bash

   ./build/bin/seal input.bc [options]

The tool writes the lifted symbolic automaton as DOT graphs for visualization.
