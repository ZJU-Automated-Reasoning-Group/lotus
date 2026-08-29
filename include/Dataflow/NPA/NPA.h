/**********************************************************************
 * Newtonian Program Analysis (NPA) – generic C++17 interface
 *
 * Implements three related layers over ω-continuous semirings:
 * - Kleene solving: solve `X = f(X)` directly by repeated evaluation.
 * - JACM Newton/NPA: solve `X = f(X)` by outer Newton iteration, where each
 *   round solves the least solution of the linearized system
 *   `Df|ν^(i)(X) + δ^(i) = X`.
 * - TOPLAS tensor-product regularization: an optional inner-solver
 *   specialization for certain LCFL linearized systems and tensor-enabled
 *   domains.
 *
 * The tensor path is not a separate outer solver. It is only an optional
 * backend for the inner linearized system that appears inside Newton/NPA.
 * When extend (⊗) is non-commutative, that linearized system may have LCFL
 * structure; then the implementation can use SCC/worklist solving or, if the
 * domain opts in, tensor-product regularization (paired semiring ->
 * left-linear -> project back).
 *
 * References:
 * - Esparza et al., "Newtonian Program Analysis" (JACM): differential Df|ν,
 *   Newton sequence, convergence to least fixed point.
 * - Reps et al., "Newtonian Program Analysis via Tensor Product" (TOPLAS
 *   2016): LCFL sub-problems, regularization via tensor product (Alg. 3.4).
 *
 * Based on OCaml NPA-PMA by Di Wang.
 *
 * Implementation split:
 *   - Core/Domain.h                : domain concept and generic operations
 *   - Core/Symbol.h                : equation symbols
 *   - Core/Expr/Expressions.h      : immutable polynomial/linearized ASTs
 *   - Core/Expr/Eval.h             : evaluation with per-run external caches
 *   - Solver/KleeneSolver.h        : public Kleene solver
 *   - Solver/NPASolver.h           : public Newton/NPA façade
 *   - Solver/Newton/               : differentiation and Newton machinery
 *   - Solver/Newton/Linear/        : SCC structure and ordinary backends
 *   - Solver/Newton/Linear/Tensor/ : optional tensor backend
 *   - LLVM/                        : LLVM-specific analysis infrastructure
 *********************************************************************/
#ifndef NPA_HPP
#define NPA_HPP

#include "Dataflow/NPA/Solver/KleeneSolver.h"
#include "Dataflow/NPA/Solver/NPASolver.h"

#endif /* NPA_HPP */
