#pragma once

/**
 * @file SymbolicAbstraction.h
 * @brief Core algorithms for automatic abstraction of bit-vector formulae
 *
 * ⚠️ **IMPORTANT**: This is a FORMULA-LEVEL abstraction library for SMT
 * formulas. This is NOT the same as `Verification/SymAbsAI`, which
 * is a PROGRAM-LEVEL analysis framework for LLVM IR.
 *
 * This module implements algorithms from "Automatic Abstraction of Bit-Vector
 * Formulae" for computing symbolic abstractions of SMT bit-vector formulas
 * using SMT solvers.
 *
 * **Key Characteristics:**
 * - Works on SMT formulas (Z3 bit-vector expressions), NOT LLVM IR
 * - Converts bit-vectors to linear integer formulas for approximation
 * - Provides abstraction algorithms (α_oct^V, α_zone^V, etc.) that take
 * formulas and return constraints
 * - Lower-level, mathematical/algorithmic library
 *
 * **When to use this library:**
 * - You have SMT formulas (bit-vectors) that need abstraction
 * - You need to approximate bit-vector constraints with linear integer
 * constraints
 * - You're working at the formula/solver level, not the program level
 *
 * **When to use Verification/SymAbsAI instead:**
 * - You're analyzing LLVM IR programs
 * - You need a complete abstract interpretation framework with fixpoint engines
 * - You want to integrate analysis into LLVM optimization passes
 *
 * Key algorithms:
 * - Algorithm 7: α_lin-exp - Linear expression maximization
 * - Algorithm 8: α_oct^V - Octagonal abstraction
 * - α_zone^V - Zone abstraction (Difference Bound Matrices)
 * - Algorithm 9: α_conv^V - Convex polyhedral abstraction
 * - Algorithm 10: relax-conv - Relaxing convex polyhedra
 * - Algorithm 11: α_a-cong - Arithmetical congruence abstraction
 * - Algorithm 12: α_aff^V - Affine equality abstraction
 * - Algorithm 13: α_poly^V - Polynomial abstraction
 *
 * This is a convenience header that includes all symbolic abstraction headers.
 * For better compile times, include only the specific headers you need:
 * - Solvers/SMT/SymAbs/Config.h - Configuration types
 * - Solvers/SMT/SymAbs/LinearExpression.h - Linear expression maximization
 * - Solvers/SMT/SymAbs/Octagon.h - Octagonal abstraction
 * - Solvers/SMT/SymAbs/Zone.h - Zone abstraction (DBM)
 * - Solvers/SMT/SymAbs/Affine.h - Affine equality abstraction
 * - Solvers/SMT/SymAbs/Polyhedron.h - Convex polyhedral abstraction
 * - Solvers/SMT/SymAbs/Congruence.h - Congruence abstraction
 * - Solvers/SMT/SymAbs/Polynomial.h - Polynomial abstraction
 */

#include "Solvers/SMT/SymAbs/Affine.h"
#include "Solvers/SMT/SymAbs/Config.h"
#include "Solvers/SMT/SymAbs/Congruence.h"
#include "Solvers/SMT/SymAbs/LinearExpression.h"
#include "Solvers/SMT/SymAbs/Octagon.h"
#include "Solvers/SMT/SymAbs/Polyhedron.h"
#include "Solvers/SMT/SymAbs/Polynomial.h"
#include "Solvers/SMT/SymAbs/Zone.h"
