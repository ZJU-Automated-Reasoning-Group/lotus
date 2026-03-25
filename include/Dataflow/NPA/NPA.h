/**********************************************************************
 * Newtonian Program Analysis (NPA) – generic C++14 header
 *
 * Implements Newton-style program analysis over ω-continuous semirings:
 * - Kleene iteration: κ^(i+1) = f(κ^(i)).
 * - Newton iteration: ν^(i+1) = ν^(i) ⊔ Δ^(i), where Δ^(i) is the least
 *   solution of the \e linearized system Df|ν^(i)(X) + δ^(i) = X.
 *
 * The linearized system is an LCFL equation system when extend (⊗) is
 * non-commutative; it can be solved by SCC decomposition with local worklists,
 * or by tensor-product regularization (paired semiring → left-linear →
 * project back).
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
 *   - Core/NPACommon.h         : domain concept (semiring) + LinearStrategy
 *   - Core/Expressions.h       : Exp0 (polynomial) / Exp1 (linearized) AST
 *   - Core/Fixpoint.h         : fix / fix_vec (Kleene-like iteration)
 *   - Core/Eval.h             : I0 (Exp0) / I1 (Exp1) evaluators
 *   - Core/Diff.h              : differential Df|ν construction
 *   - Core/LCFLDetector.h     : detect LCFL structure (Concat/Star)
 *   - Core/LinearSolvers.h    : SCC-based and tensor linear solvers
 *   - Core/TensorLinearSolve.h: tensor-product solver (Alg. 3.4)
 *   - Core/Solver.h           : KleeneIter / NewtonIter, Solver<D,ITER>
 *********************************************************************/
#ifndef NPA_HPP
#define NPA_HPP

#include "Dataflow/NPA/Core/Solver.h"

#endif /* NPA_HPP */
