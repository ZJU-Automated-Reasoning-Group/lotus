/**
 * @file Octagon.cpp
 * @brief Implementation of Algorithm 8: α_oct^V
 *
 * This module implements Algorithm 8 from "Automatic Abstraction of Bit-Vector
 * Formulae" for computing the octagonal abstraction of a set of bit-vectors V
 * subject to formula φ.
 *
 * **Octagonal Domain:**
 * The octagon domain is a relational numerical abstract domain that can express
 * constraints of the form ±x ± y ≤ c, where x and y are variables and c is a
 * constant. The coefficients λ_i, λ_j ∈ {-1, 1} restrict the constraints to
 * simple linear combinations.
 *
 * **Mathematical Formulation:**
 * An octagon over variables V = {v_1, ..., v_n} is a conjunction of
 * constraints: ±⟨v_i⟩ ± ⟨v_j⟩ ≤ d where:
 * - ± indicates either +1 or -1 (coefficients are in {-1, 0, 1})
 * - ⟨v_i⟩ is the integer interpretation of bit-vector v_i
 * - d is a constant bound
 * - For unary constraints (i == j), we get ±⟨v_i⟩ ≤ d or ±2·⟨v_i⟩ ≤ d
 *
 * **Algorithm Overview:**
 * The algorithm iterates over all pairs (v_i, v_j) of variables and all
 * combinations of coefficients (λ_i, λ_j) ∈ {(-1,-1), (-1,1), (1,-1), (1,1)}.
 * For each combination, it computes the least upper bound using α_lin-exp
 * (Algorithm 7). For unary constraints (i == j), it normalizes by dividing by 2
 * since we get 2·v_i ≤ d.
 *
 * **Properties:**
 * - More expressive than zones (which only allow x - y ≤ c)
 * - Less expressive than polyhedra (which allow arbitrary linear combinations)
 * - Polynomial time complexity: O(n²) where n is the number of variables
 * - Widely used in numerical static analysis for overflow detection
 *
 * **Use Cases:**
 * - Numerical invariant discovery
 * - Overflow detection in integer arithmetic
 * - General relational numerical analysis
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include <llvm/ADT/Optional.h>
#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {

/**
 * @brief Convert an octagonal constraint to a Z3 integer inequality expression.
 *
 * This function converts an OctagonalConstraint structure into its
 * corresponding Z3 expression. The conversion handles both unary constraints
 * (single variable) and binary constraints (two variables).
 *
 * **Constraint Forms:**
 * - Unary: ±λ_i · ⟨v_i⟩ ≤ d  (when c.unary == true)
 * - Binary: λ_i · ⟨v_i⟩ + λ_j · ⟨v_j⟩ ≤ d  (when c.unary == false)
 *
 * @param c The octagonal constraint to convert
 * @return Z3 expression representing the constraint as an integer inequality
 */
z3::expr oct_constraint_to_expr(const OctagonalConstraint &c) {
  context &ctx = c.var_i.ctx();
  expr lhs = ctx.int_val(0);
  lhs = lhs + ctx.int_val(c.lambda_i) * SymAbs::bv_signed_to_int(c.var_i);
  if (!c.unary) {
    lhs = lhs + ctx.int_val(c.lambda_j) * SymAbs::bv_signed_to_int(c.var_j);
  }
  return lhs <= ctx.int_val(c.bound);
}

/**
 * @brief Compute the octagonal abstraction α_oct^V(φ)
 *
 * Algorithm 8: Computes the least octagon describing the set of bit-vectors V
 * subject to formula φ. The algorithm systematically explores all possible
 * octagonal constraints over the variables in V.
 *
 * **Algorithm Steps:**
 * 1. For each pair (v_i, v_j) of variables (including i == j for unary
 * constraints)
 * 2. For each coefficient combination (λ_i, λ_j) ∈ {(-1,-1), (-1,1), (1,-1),
 * (1,1)}
 * 3. Build a linear expression λ_i · v_i + λ_j · v_j
 * 4. Use α_lin-exp (Algorithm 7) to compute the least upper bound
 * 5. For unary constraints (i == j), normalize by dividing by 2
 * 6. Collect all valid constraints into the result
 *
 * **Normalization for Unary Constraints:**
 * When i == j and λ_i == λ_j, we get 2·λ_i · v_i ≤ d. This is normalized to
 * λ_i · v_i ≤ ⌊d/2⌋ by using floor division, which ensures soundness (the
 * constraint is at least as strong as the original).
 *
 * **Example:**
 * For variables {x, y} and formula φ = (x ≥ 0) ∧ (x ≤ 10) ∧ (y ≥ x), the
 * algorithm would produce constraints like:
 * - x ≤ 10, -x ≤ 0 (from unary constraints on x)
 * - y - x ≤ 0 (from binary constraint x - y)
 * - etc.
 *
 * @param phi The formula constraining the variables (bit-vector SMT formula)
 * @param variables The set of bit-vector variables V = {v_1, ..., v_n}
 * @param config Configuration including timeout and iteration limits
 * @return Vector of octagonal constraints representing the least octagon
 *
 * @note The algorithm has O(n²) complexity where n is the number of variables,
 *       as it considers all pairs of variables and 4 coefficient combinations
 * per pair.
 * @note Constraints that cannot be bounded (α_lin-exp returns None) are
 * skipped.
 */
std::vector<OctagonalConstraint>
alpha_oct_V(z3::expr phi, const std::vector<z3::expr> &variables,
            const AbstractionConfig &config) {

  std::vector<OctagonalConstraint> constraints;

  if (variables.empty()) {
    return constraints;
  }

  context &ctx = phi.ctx();

  // Algorithm 8: Iterate over all pairs (v_i, v_j) and all (λ_i, λ_j) ∈ {-1, 1}
  // × {-1, 1} We iterate j from i to avoid duplicates (since ±x ± y covers all
  // cases)
  for (size_t i = 0; i < variables.size(); ++i) {
    for (size_t j = i; j < variables.size(); ++j) {
      // All possible coefficient combinations for octagonal constraints
      std::vector<std::pair<int, int>> coeff_pairs = {
          {1, 1},  // v_i + v_j ≤ d
          {1, -1}, // v_i - v_j ≤ d
          {-1, 1}, // -v_i + v_j ≤ d
          {-1, -1} // -v_i - v_j ≤ d
      };

      for (const auto &pair : coeff_pairs) {
        int lambda_i = pair.first;
        int lambda_j = pair.second;
        LinearExpression lexpr;

        if (i == j) {
          // Unary constraint case: ±v_i ± v_i
          // When λ_i != λ_j, the terms cancel (e.g., v_i - v_i = 0), so skip
          // When λ_i == λ_j, we get ±2·v_i, which we'll normalize later
          if (lambda_i != lambda_j) {
            continue;
          }
          lexpr.variables.push_back(variables[i]);
          lexpr.coefficients.push_back(2 * lambda_i);
        } else {
          // Binary constraint case: λ_i · v_i + λ_j · v_j
          lexpr.variables.push_back(variables[i]);
          lexpr.coefficients.push_back(lambda_i);
          lexpr.variables.push_back(variables[j]);
          lexpr.coefficients.push_back(lambda_j);
        }

        // Compute the least upper bound using Algorithm 7
        auto bound = alpha_lin_exp(phi, lexpr, config);
        if (!bound.hasValue()) {
          // Cannot compute bound, skip this constraint
          continue;
        }

        if (i == j) {
          // Normalize unary constraint: 2·λ_i · v_i ≤ d  ⇒  λ_i · v_i ≤ ⌊d/2⌋
          // Using floor division ensures soundness (the normalized constraint
          // is at least as strong as the original)
          int64_t normalized = SymAbs::div_floor(bound.getValue(), 2);
          constraints.emplace_back(variables[i], lambda_i, normalized);
        } else {
          // Binary constraint: λ_i · v_i + λ_j · v_j ≤ d
          constraints.emplace_back(variables[i], variables[j], lambda_i,
                                   lambda_j, bound.getValue());
        }
      }
    }
  }

  return constraints;
}

} // namespace SymAbs
