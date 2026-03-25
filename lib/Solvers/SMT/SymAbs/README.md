# SMT Formula Abstraction Library (SymAbs)

## Overview

This module implements algorithms from "Automatic Abstraction of Bit-Vector Formulae" for computing symbolic abstractions of **SMT formulas** (specifically bit-vector formulas). 

**⚠️ Important Distinction**: This library is **NOT** the same as `lib/Verification/SymAbsAI`, which is a complete program analysis framework for LLVM IR.

### Key Differences

| Aspect | `lib/Solvers/SMT/SymAbs` (this library) | `lib/Verification/SymAbsAI` |
|--------|------------------------------------------|----------------------------------------|
| **Input** | SMT bit-vector formulas (Z3 expressions) | LLVM IR (program code) |
| **Output** | Abstract constraints (intervals, octagons, etc.) | Abstract domain values for LLVM values |
| **Approximation** | Converts bit-vectors to **linear integer formulas** | Works directly with program semantics |
| **Level** | Formula-level abstraction algorithms | Program-level abstract interpretation |
| **Integration** | Standalone SMT formula processing | Integrated LLVM pass with fixpoint engine |
| **Use Case** | Abstracting SMT formulas for constraint solving | Static analysis and optimization of programs |

### Why Linear Integer Approximation?

This library uses linear integer formulas to approximate bit-vector formulas because:
- Bit-vector arithmetic is complex (wrap-around, modular arithmetic)
- Linear integer arithmetic is more tractable for SMT solvers
- Allows reuse of efficient integer constraint algorithms
- **Trade-off**: May lose precision due to the approximation

The conversion is done via `bv_signed_to_int()` which interprets bit-vectors in two's complement to avoid wrap-around during arithmetic.

## Abstract Domains

This module implements several abstract domains for symbolic abstraction of **SMT formulas**:

### Zone Domain (Difference Bound Matrices)
- **File**: `Zone.cpp`, `Zone.h`
- **Constraints**: `x - y ≤ c` and `x ≤ c`
- **Description**: The zone domain (also known as Difference Bound Matrices or DBM) tracks difference constraints between variables. It is more restrictive than the octagon domain but computationally more efficient.
- **Use cases**: Timing analysis, scheduling, real-time systems verification

### Octagon Domain
- **File**: `Octagon.cpp`, `Octagon.h`
- **Constraints**: `±x ± y ≤ c`
- **Description**: The octagon domain is a relational numerical abstract domain that can express constraints involving linear combinations of at most two variables with coefficients in {-1, 0, 1}.
- **Use cases**: General numerical analysis, overflow detection

### Other Domains
- **Intervals**: Represented by unary constraints
- **Polyhedra**: General convex constraints
- **Affine Equalities**: Linear equality relations
- **Congruences**: Modular arithmetic constraints
- **Polynomials**: Non-linear polynomial constraints

## Key Algorithms

- **Algorithm 7**: α_lin-exp - Linear expression maximization
- **Algorithm 8**: α_oct^V - Octagonal abstraction
- **α_zone^V**: Zone abstraction (Difference Bound Matrices)
- **Algorithm 9**: α_conv^V - Convex polyhedral abstraction
- **Algorithm 10**: relax-conv - Relaxing convex polyhedra
- **Algorithm 11**: α_a-cong - Arithmetical congruence abstraction
- **Algorithm 12**: α_aff^V - Affine equality abstraction
- **Algorithm 13**: α_poly^V - Polynomial abstraction

## Usage

This library operates on Z3 expressions (bit-vector formulas):

```cpp
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"

z3::context ctx;
z3::expr x = ctx.bv_const("x", 8);
z3::expr phi = (x >= 0) && (x <= 10);

// Compute maximum value (uses linear integer approximation)
auto max_val = SymAbs::maximum(phi, x);
```

## When to Use This Library

Use `lib/Solvers/SMT/SymAbs` when:
- You have SMT formulas (bit-vectors) that need abstraction
- You need to approximate bit-vector constraints with linear integer constraints
- You're working at the formula/solver level, not the program level

Use `lib/Verification/SymAbsAI` when:
- You're analyzing LLVM IR programs
- You need a complete abstract interpretation framework
- You want to integrate analysis into LLVM optimization passes

