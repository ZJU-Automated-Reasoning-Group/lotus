FPsolve — Fixed-Point Equation Solver
======================================

FPsolve is a fixed-point equation solver based on Newton's method
generalized to omega-continuous semirings. It solves systems of equations
over a variety of semiring domains, supporting both Kleene iteration
(linear equations) and Newton's method (polynomial equations).

**Location**: ``third-party/fpsolve/`` (vendored, opt-in)

**Build toggle**: ``-DLOTUS_ENABLE_FPSOLVE=ON``

**Dependencies**: Boost (auto-downloaded if not found)

Overview
--------

FPsolve represents equation systems over a set of variables and semiring
elements. Its solver engine supports:

- **Kleene semi-naive iteration** for linear equation systems
- **Newton's method** generalized to omega-continuous semirings for
  polynomial equation systems

Semiring Domains
----------------

The library provides a wide range of built-in semirings:

.. code-block:: text

   BooleanSemiring         — standard Boolean (∧, ∨)
   TropicalSemiring        — min-plus (tropical) semiring
   ViterbiSemiring         — max-probability semiring
   FloatSemiring           — floating-point semiring
   FreeSemiring            — formal language (free monoid)
   CommutativeRExp         — commutative regular expressions
   PreciseRationalSemiring — exact rational arithmetic
   MaxMinSemiring          — max-min fuzzy logic semiring
   LossySemiring           — lossy channel automaton semiring
   LossyFiniteAutomaton    — lossy finite automaton domain
   PrefixSemiring          — prefix-order semiring
   TupleSemiring           — product (tuple) of semirings
   SemilinearSet           — semilinear set representation
   PseudoLinearSet         — pseudo-linear set (NDD-based)
   SemilinSetNdd           — semilinear set with NDD backend

Directory Structure
-------------------

::

   third-party/fpsolve/
   ├── include/fpsolve/
   │   ├── fpsolve.h              # Top-level solver interface
   │   ├── parser.h               # Grammar/equation file parser
   │   ├── datastructs/           # Variable, free-structure, var-degree-map
   │   ├── polynomials/           # Commutative/non-commutative polynomial representation
   │   ├── semirings/             # Semiring domain implementations
   │   ├── solvers/               # Kleene, Newton solver engines
   │   └── utils/                 # Debug, profiling, string utilities
   ├── lib/fpsolve/               # Implementation sources
   ├── tools/                     # Standalone solver binaries
   ├── tests/                     # Unit tests and grammar benchmarks
   └── scripts/                   # Benchmark and conversion scripts

Build and Run
-------------

.. code-block:: bash

   cmake -S . -B build -DLOTUS_ENABLE_FPSOLVE=ON
   cmake --build build -j$(nproc)
