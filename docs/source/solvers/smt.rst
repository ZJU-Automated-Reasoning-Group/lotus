SMT (Satisfiability Modulo Theories)
===================================

SMT solving infrastructure with Z3 integration and support for common theories.

Overview
--------

The SMT backend provides a unified interface for building and solving SMT
formulas over a variety of theories.

**Location**: ``lib/Solvers/SMT/``

**Main capabilities**:

- Z3-based SMT solver backend.
- Theory support for arithmetic, arrays, bit-vectors, and uninterpreted
  functions.
- Utilities for incremental solving and formula construction.

Typical Use Cases
-----------------

- Encoding verification conditions and safety properties.
- Solving constrained Horn clauses (CHCs) produced by analyses and front-ends.
- Implementing symbolic execution and path feasibility checks.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <Solvers/SMT/SMTContext.h>

   SMTContext Ctx;
   auto Solver = Ctx.createSolver();
   auto Phi = /* build formula */;
   Solver->add(Phi);

Features
--------

- **Z3 integration** – Tight integration with Z3 4.11 as the primary SMT
  backend.
- **Theory solvers** – Support for integer and real arithmetic, arrays,
  bit-vectors, and uninterpreted functions.
- **Formula construction** – Helper APIs for building and manipulating SMT
  expressions.
- **Incremental solving** – Push/pop interfaces for iterative solving.

SymAbs: SMT Formula Abstraction Library
----------------------------------------

**Location**: ``lib/Solvers/SMT/SymAbs/``

**Important Distinction**: This library is **NOT** the same as
:doc:`../verification/symabs-ai`, which is a complete program
analysis framework for LLVM IR.

SymAbs implements algorithms from "Automatic Abstraction of Bit-Vector Formulae"
for computing symbolic abstractions of **SMT formulas** (specifically bit-vector
formulas). It operates at the formula/solver level, not the program level.

**Key Differences**:

+------------------+------------------------------------------+----------------------------------------+
| Aspect           | ``lib/Solvers/SMT/SymAbs``              | ``lib/Verification/SymAbsAI``|
+==================+==========================================+========================================+
| **Input**        | SMT bit-vector formulas (Z3 expressions) | LLVM IR (program code)                |
+------------------+------------------------------------------+----------------------------------------+
| **Output**       | Abstract constraints (intervals,         | Abstract domain values for LLVM values |
|                  | octagons, etc.)                          |                                        |
+------------------+------------------------------------------+----------------------------------------+
| **Approximation**| Converts bit-vectors to linear integer   | Works directly with program semantics  |
|                  | formulas                                 |                                        |
+------------------+------------------------------------------+----------------------------------------+
| **Level**        | Formula-level abstraction algorithms     | Program-level abstract interpretation  |
+------------------+------------------------------------------+----------------------------------------+
| **Integration**  | Standalone SMT formula processing        | Integrated LLVM pass with fixpoint     |
|                  |                                          | engine                                 |
+------------------+------------------------------------------+----------------------------------------+
| **Use Case**     | Abstracting SMT formulas for constraint  | Static analysis and optimization of    |
|                  | solving                                  | programs                               |
+------------------+------------------------------------------+----------------------------------------+

**Abstract Domains**:

- **Zone Domain** (Difference Bound Matrices): Constraints of the form
  ``x - y ≤ c`` and ``x ≤ c``. More restrictive than octagons but more efficient.
- **Octagon Domain**: Constraints of the form ``±x ± y ≤ c``. Relational
  numerical abstract domain.
- **Intervals**: Unary constraints representing value ranges
- **Polyhedra**: General convex constraints
- **Affine Equalities**: Linear equality relations
- **Congruences**: Modular arithmetic constraints
- **Polynomials**: Non-linear polynomial constraints

**Why Linear Integer Approximation?**

SymAbs uses linear integer formulas to approximate bit-vector formulas because:

- Bit-vector arithmetic is complex (wrap-around, modular arithmetic)
- Linear integer arithmetic is more tractable for SMT solvers
- Allows reuse of efficient integer constraint algorithms
- **Trade-off**: May lose precision due to the approximation

The conversion uses ``bv_signed_to_int()`` which interprets bit-vectors in
two's complement to avoid wrap-around during arithmetic.

**When to Use**:

- Use ``lib/Solvers/SMT/SymAbs`` when:
  - You have SMT formulas (bit-vectors) that need abstraction
  - You need to approximate bit-vector constraints with linear integer constraints
  - You're working at the formula/solver level, not the program level

- Use ``lib/Verification/SymAbsAI`` when:
  - You're analyzing LLVM IR programs
  - You need a complete abstract interpretation framework
  - You want to integrate analysis into LLVM optimization passes

Integration Notes
-----------------

The SMT backend is used throughout Lotus by components that require reasoning
over rich theories, including CHC-based verifiers and symbolic abstractions.
See :doc:`index` for a high-level overview of where the SMT solver fits in
the rest of the system.


