/**
 * @file LinearExpression.cpp
 * @brief Implementation of Algorithm 7: α_lin-exp
 *
 * This module implements Algorithm 7 from "Automatic Abstraction of Bit-Vector
 * Formulae" for computing the least upper bound of a linear expression Σ λ_i ·
 * ⟨⟨v_i⟩⟩ subject to a formula φ.
 *
 * **Key Design Decisions:**
 * - **Unbounded Integer Representation**: We convert bit-vectors to unbounded
 * integers using bv_signed_to_int() to avoid wrap-around issues. This
 * interprets bit-vectors in two's complement but represents them as unbounded
 * integers during computation.
 *
 * - **Optimization Strategy**: Uses Z3's optimize engine as the primary method
 * for finding exact optima. This leverages Z3's internal optimization
 * capabilities.
 *
 * - **Fallback Mechanism**: If the optimize engine returns unknown or
 * non-numeral values, we fall back to a bounded ascending search that
 * iteratively finds better solutions until no improvement can be found. This
 * ensures robustness when optimization fails.
 *
 * **Algorithm Overview:**
 * 1. Convert the linear expression to an unbounded integer representation
 * 2. Use Z3's optimize engine to maximize the expression subject to φ
 * 3. If successful, extract and return the maximum value
 * 4. If the optimizer fails or returns unknown, use iterative linear search:
 *    - Find an initial satisfying model
 *    - Iteratively constrain the expression to be greater than current best
 *    - Stop when no better solution exists
 *
 * **Time Complexity**: O(k) where k is the number of iterations in the worst
 * case (linear search). The optimize engine typically performs much better.
 *
 * **Precision**: The result is exact (subject to SMT solver precision) when
 * using the optimize engine, or an approximation bounded by iteration limits
 * for fallback.
 */

#include "Solvers/SMT/SymAbs/LinearExpression.h"

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <cassert>

#include <llvm/ADT/Optional.h>
#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {

/**
 * @brief Build an unbounded integer representation of Σ λ_i · ⟨⟨v_i⟩⟩.
 *
 * This function converts a linear expression over bit-vector variables to an
 * equivalent expression over unbounded integers. Each bit-vector variable v_i
 * is converted using bv_signed_to_int(), which interprets the bit-vector in
 * two's complement representation but produces an unbounded integer value.
 *
 * **Example**: For a 4-bit variable with value 0b1111 (which is -1 in two's
 * complement), bv_signed_to_int() produces the integer -1, not 15.
 *
 * @param lexpr The linear expression to convert (variables and coefficients)
 * @param ctx The Z3 context
 * @return Z3 expression representing the linear combination as an unbounded
 * integer
 *
 * @pre lexpr.variables.size() == lexpr.coefficients.size()
 * @pre !lexpr.variables.empty()
 */
static expr build_integer_linear_expr(const LinearExpression &lexpr,
                                      context &ctx) {
  assert(!lexpr.variables.empty());
  assert(lexpr.variables.size() == lexpr.coefficients.size());

  expr sum = ctx.int_val(0);
  for (size_t i = 0; i < lexpr.variables.size(); ++i) {
    expr v_int = SymAbs::bv_signed_to_int(lexpr.variables[i]);
    int64_t coeff = lexpr.coefficients[i];
    // Skip zero coefficients to avoid unnecessary terms
    if (coeff == 0)
      continue;
    sum = sum + ctx.int_val(coeff) * v_int;
  }
  return sum;
}

/**
 * @brief Compute the least upper bound of a linear expression subject to a
 * formula.
 *
 * Algorithm 7: α_lin-exp^V(φ, Σ λ_i · ⟨⟨v_i⟩⟩)
 *
 * This function computes the maximum value that the linear expression
 * Σ λ_i · ⟨⟨v_i⟩⟩ can take over all models satisfying φ.
 *
 * **Implementation Strategy:**
 * 1. **Primary Method (Z3 Optimize)**: Uses Z3's optimize engine to find the
 * exact maximum. This is the preferred method as it leverages Z3's internal
 * optimization algorithms and typically converges quickly.
 *
 * 2. **Fallback Method (Iterative Search)**: If the optimize engine returns
 * unknown or fails to produce a numeral value, we fall back to a bounded
 * iterative search:
 *    - Start with an initial satisfying model
 *    - In each iteration, add a constraint requiring the expression to be
 * greater than the current best value
 *    - Continue until no better solution exists (UNSAT) or iteration limit
 * reached
 *
 * **Termination:**
 * - Returns llvm::None if φ is unsatisfiable
 * - Returns llvm::None if no valid integer value can be extracted
 * - Returns the maximum value (least upper bound) on success
 * - May return a sub-optimal value if max_iterations is exceeded
 *
 * @param phi The formula constraining the variables (bit-vector SMT formula)
 * @param lexpr The linear expression to maximize (Σ λ_i · ⟨⟨v_i⟩⟩)
 * @param config Configuration including timeout and iteration limits
 * @return The least upper bound d ∈ Z, or llvm::None if computation fails
 *
 * @note The result is computed over unbounded integers (via bv_signed_to_int),
 *       which avoids wrap-around issues present in bit-vector arithmetic.
 * @note This function may be computationally expensive for complex formulas,
 *       especially when the fallback iterative search is triggered.
 */
llvm::Optional<int64_t> alpha_lin_exp(const z3::expr &phi,
                                      const LinearExpression &lexpr,
                                      const AbstractionConfig &config) {
  if (lexpr.variables.empty()) {
    return llvm::None;
  }

  context &ctx = phi.ctx();
  expr int_expr = build_integer_linear_expr(lexpr, ctx);

  // First attempt: use Z3 optimize to obtain a model with maximal value.
  // This leverages Z3's internal optimization engine for efficient solving.
  optimize opt(ctx);
  params p(ctx);
  p.set("timeout", config.timeout_ms);
  opt.set(p);
  opt.add(phi);
  opt.maximize(int_expr);

  auto check_res = opt.check();
  if (check_res == sat) {
    // Success: extract the optimal value from the model
    model m = opt.get_model();
    expr val = m.eval(int_expr, true);
    auto as_int = SymAbs::to_int64(val);
    if (as_int.hasValue()) {
      return as_int;
    }
  }

  if (check_res == unsat) {
    // Formula is unsatisfiable, so no maximum exists
    return llvm::None;
  }

  // Fallback: bounded ascending search using a plain solver when optimize
  // returns unknown or non-numeral values. This ensures we still get a result
  // even when optimization fails, though it may be less optimal.
  solver sol(ctx);
  sol.set(p);
  sol.add(phi);

  // Find an initial satisfying model to start the search
  if (sol.check() != sat) {
    return llvm::None;
  }

  model m0 = sol.get_model();
  expr v0 = m0.eval(int_expr, true);
  auto best_opt = SymAbs::to_int64(v0);
  if (!best_opt.hasValue()) {
    return llvm::None;
  }

  int64_t best = best_opt.getValue();
  unsigned iter = 0;

  // Iterative improvement: keep searching for better values until
  // no improvement is possible or iteration limit is reached
  while (iter < config.max_iterations) {
    sol.push(); // Save state for backtracking
    // Add constraint requiring expression to be greater than current best
    sol.add(int_expr > ctx.int_val(best));
    auto res = sol.check();
    if (res != sat) {
      // No better solution exists (UNSAT), so current best is optimal
      sol.pop();
      break;
    }
    // Found a better solution, update best and continue
    model m_new = sol.get_model();
    expr v_new = m_new.eval(int_expr, true);
    auto v_int = SymAbs::to_int64(v_new);
    sol.pop(); // Restore state
    if (!v_int.hasValue()) {
      // Cannot extract integer value, stop search
      break;
    }
    best = v_int.getValue();
    ++iter;
  }

  return best_opt.hasValue() ? llvm::Optional<int64_t>(best) : llvm::None;
}

/**
 * @brief Compute the minimum value of a variable subject to a formula.
 *
 * This is a convenience function that computes the minimum value of a variable
 * by leveraging the linear expression maximization algorithm. The key insight
 * is that minimizing v is equivalent to maximizing -v, so we can reuse
 * alpha_lin_exp() with a negated coefficient.
 *
 * **Mathematical relationship:**
 *   min_{v | φ} v = -max_{v | φ} (-v)
 *
 * @param phi The formula constraining the variable
 * @param variable The variable to minimize
 * @param config Configuration for the algorithm
 * @return The minimum value, or llvm::None if computation fails
 */
llvm::Optional<int64_t> minimum(const z3::expr &phi, const z3::expr &variable,
                                const AbstractionConfig &config) {
  // Minimize v by maximizing -v (reuse alpha_lin_exp with negated coefficient)
  LinearExpression neg_expr;
  neg_expr.variables.push_back(variable);
  neg_expr.coefficients.push_back(-1);

  auto max_neg = alpha_lin_exp(phi, neg_expr, config);
  if (!max_neg.hasValue()) {
    return llvm::None;
  }

  // Negate the result to get the minimum
  return -max_neg.getValue();
}

/**
 * @brief Compute the maximum value of a variable subject to a formula.
 *
 * This is a convenience function that computes the maximum value of a single
 * variable. It's equivalent to alpha_lin_exp() with a linear expression
 * containing only the variable with coefficient 1.
 *
 * @param phi The formula constraining the variable
 * @param variable The variable to maximize
 * @param config Configuration for the algorithm
 * @return The maximum value, or llvm::None if computation fails
 */
llvm::Optional<int64_t> maximum(const z3::expr &phi, const z3::expr &variable,
                                const AbstractionConfig &config) {
  // Build linear expression: 1 * variable
  LinearExpression expr;
  expr.variables.push_back(variable);
  expr.coefficients.push_back(1);

  return alpha_lin_exp(phi, expr, config);
}

} // namespace SymAbs
