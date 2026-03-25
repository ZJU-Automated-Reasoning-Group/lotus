SMT Formula Abstraction (SymAbs)
================================

This section documents the SMT Formula Abstraction library (``SymAbs``), which
implements algorithms for computing symbolic abstractions of SMT bit-vector formulas.

Overview
--------

**Location**: ``lib/Solvers/SMT/SymAbs/``

**Important**: This library is distinct from ``lib/Verification/SymAbsAI``.
SymAbs operates on **SMT formulas**, while the verification module operates on
**LLVM IR programs**.

Key Differences
----------------

+----------------------+------------------------------------------+----------------------------------------+
| Aspect               | ``lib/Solvers/SMT/SymAbs``               | ``lib/Verification/SymAbsAI``|
+======================+==========================================+========================================+
| **Input**            | SMT bit-vector formulas (Z3 expressions) | LLVM IR (program code)                |
+----------------------+------------------------------------------+----------------------------------------+
| **Output**           | Abstract constraints (intervals,          | Abstract domain values for LLVM values |
|                      | octagons, etc.)                          |                                        |
+----------------------+------------------------------------------+----------------------------------------+
| **Approximation**    | Converts bit-vectors to linear integer    | Works directly with program semantics  |
|                      | formulas                                 |                                        |
+----------------------+------------------------------------------+----------------------------------------+
| **Level**            | Formula-level abstraction algorithms     | Program-level abstract interpretation  |
+----------------------+------------------------------------------+----------------------------------------+
| **Integration**      | Standalone SMT formula processing        | Integrated LLVM pass with fixpoint     |
|                      |                                          | engine                                 |
+----------------------+------------------------------------------+----------------------------------------+
| **Use Case**         | Abstracting SMT formulas for constraint  | Static analysis and optimization of    |
|                      | solving                                  | programs                               |
+----------------------+------------------------------------------+----------------------------------------+

Why Linear Integer Approximation?
---------------------------------

SymAbs uses linear integer formulas to approximate bit-vector formulas because:

- Bit-vector arithmetic is complex (wrap-around, modular arithmetic)
- Linear integer arithmetic is more tractable for SMT solvers
- Allows reuse of efficient integer constraint algorithms
- **Trade-off**: May lose precision due to the approximation

The conversion uses ``bv_signed_to_int()`` which interprets bit-vectors in
two's complement to avoid wrap-around during arithmetic.

Abstract Domains
----------------

SymAbs implements several abstract domains for symbolic abstraction:

Zone Domain (Difference Bound Matrices)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Files**: ``Zone.cpp``, ``Zone.h``
- **Constraints**: ``x - y ≤ c`` and ``x ≤ c``
- **Description**: The zone domain tracks difference constraints between variables.
  More restrictive than octagons but computationally more efficient.
- **Use cases**: Timing analysis, scheduling, real-time systems verification

Octagon Domain
~~~~~~~~~~~~~~

- **Files**: ``Octagon.cpp``, ``Octagon.h``
- **Constraints**: ``±x ± y ≤ c``
- **Description**: Relational numerical domain expressing constraints with
  linear combinations of at most two variables with coefficients in ``{-1, 0, 1}``.
- **Use cases**: General numerical analysis, overflow detection

Other Domains
~~~~~~~~~~~~~

- **Intervals**: Unary constraints representing value ranges
- **Polyhedra**: General convex constraints
- **Affine Equalities**: Linear equality relations
- **Congruences**: Modular arithmetic constraints
- **Polynomials**: Non-linear polynomial constraints

Key Algorithms
--------------

- **Algorithm 7**: ``α_lin-exp`` - Linear expression maximization
- **Algorithm 8**: ``α_oct^V`` - Octagonal abstraction
- **α_zone^V**: Zone abstraction (Difference Bound Matrices)
- **Algorithm 9**: ``α_conv^V`` - Convex polyhedral abstraction
- **Algorithm 10**: ``relax-conv`` - Relaxing convex polyhedra
- **Algorithm 11**: ``α_a-cong`` - Arithmetical congruence abstraction
- **Algorithm 12**: ``α_aff^V`` - Affine equality abstraction
- **Algorithm 13**: ``α_poly^V`` - Polynomial abstraction

C++ API Usage
--------------

.. code-block:: cpp

   #include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"

   using namespace z3;
   using namespace SymAbs;

   context ctx;
   expr x = ctx.bv_const("x", 8);
   expr y = ctx.bv_const("y", 8);

   // Build a formula
   expr phi = (x >= 0) && (x <= 10) && (y >= 0) && (y <= 20);

   // Compute abstraction
   auto zone = OctagonAbstraction::create(phi);

   // Query abstract domain
   auto max_x = zone.maximum(x);
   auto bounds = zone.getBounds(x);

When to Use SymAbs
------------------

Use ``lib/Solvers/SMT/SymAbs`` when:

- You have SMT formulas (bit-vectors) that need abstraction
- You need to approximate bit-vector constraints with linear integer constraints
- You're working at the formula/solver level, not the program level

Use ``lib/Verification/SymAbsAI`` when:

- You're analyzing LLVM IR programs
- You need a complete abstract interpretation framework
- You want to integrate analysis into LLVM optimization passes

Related Components
------------------

- See :doc:`smtsampler` - SMT samplers (uses SymAbs for RegionSampler)
- See :doc:`smt` - Core SMT solver infrastructure
