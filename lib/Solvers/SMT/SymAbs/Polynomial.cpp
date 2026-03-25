/**
 * @file Polynomial.cpp
 * @brief Implementation of Algorithm 13: α_poly^V
 *
 * This module implements Algorithm 13 from "Automatic Abstraction of Bit-Vector
 * Formulae" for computing polynomial abstraction with template monomials.
 *
 * **Polynomial Abstraction:**
 * Polynomial abstraction extends affine abstraction to handle non-linear terms.
 * Given a set of template monomials S = {s_1, ..., s_k} (e.g., x², xy, x³,
 * ...), the algorithm computes an affine system over the extended variable set
 * (v_1, ..., v_n, s_1, ..., s_k), where the s_i represent polynomial terms.
 *
 * **Mathematical Formulation:**
 * The polynomial hull is computed by treating monomials as additional variables
 * and computing the affine hull over the extended variable set. This reduces
 * polynomial abstraction to affine abstraction over extended dimensions.
 *
 * **Algorithm Overview:**
 * Algorithm 13 follows a similar structure to Algorithm 12 (affine
 * abstraction), but operates over extended variables that include both original
 * variables and monomial terms. The algorithm:
 * 1. Extends the variable set with monomials S
 * 2. Computes affine equalities over the extended set
 * 3. Iteratively refines by finding counter-examples
 *
 * **Use Cases:**
 * - Discovering polynomial relationships (e.g., x² + y² = r²)
 * - Non-linear invariant discovery
 * - Extending affine analysis to handle non-linear terms
 *
 * **Note:** The current implementation appears to be a simplified version.
 * A complete implementation would properly handle monomial evaluation and
 * constraint generation for polynomial terms.
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <cassert>
#include <map>

#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {

/**
 * @brief Evaluate a monomial expression under a Z3 model.
 *
 * This helper function evaluates a monomial (polynomial term) under a given
 * model, extracting its integer value. The function handles:
 * - Numeric constants
 * - Variable constants
 * - Multiplications (e.g., x * y)
 *
 * **Limitations:**
 * The current implementation is simplified and handles only basic cases.
 * A complete implementation would need to handle arbitrary polynomial terms.
 *
 * @param monomial The monomial expression to evaluate
 * @param m The Z3 model providing variable values
 * @param ctx The Z3 context
 * @return The integer value of the monomial under the model
 */
static int64_t eval_monomial(const expr &monomial, const model &m,
                             context &ctx) {
  // For now, handle simple cases
  // Full implementation would need to handle arbitrary monomials

  if (monomial.is_numeral()) {
    auto as_int = SymAbs::to_int64(monomial);
    return as_int.hasValue() ? as_int.getValue() : 0;
  }

  if (monomial.is_const()) {
    expr val = m.eval(monomial, true);
    if (val.is_numeral()) {
      auto as_int = SymAbs::to_int64(val);
      return as_int.hasValue() ? as_int.getValue() : 0;
    }
  }

  // For multiplication: v1 * v2
  if (monomial.decl().decl_kind() == Z3_OP_BMUL) {
    assert(monomial.num_args() == 2);
    int64_t val1 = eval_monomial(monomial.arg(0), m, ctx);
    int64_t val2 = eval_monomial(monomial.arg(1), m, ctx);
    return val1 * val2;
  }

  // For other operations, return 0 as fallback
  return 0;
}

/**
 * @brief Compute polynomial abstraction α_poly^V(φ, S)
 *
 * Algorithm 13: Computes polynomial hull with template monomials S by reducing
 * to affine abstraction over extended variables.
 *
 * **Algorithm Overview:**
 * The algorithm treats polynomial abstraction as affine abstraction over an
 * extended variable set that includes both original variables and monomial
 * terms:
 * - Original variables: v_1, ..., v_n
 * - Monomials: s_1, ..., s_k (from template S)
 * - Extended set: (v_1, ..., v_n, s_1, ..., s_k)
 *
 * The algorithm builds an affine system [A|b] over the extended variables,
 * where each equality may involve both original variables and their polynomial
 * relationships (captured by monomials).
 *
 * **Current Implementation Status:**
 * This implementation appears to be a simplified version. A complete
 * implementation would:
 * - Properly encode monomial constraints (ensuring s_i actually equals the
 * monomial)
 * - Use full affine abstraction (Algorithm 12) over extended variables
 * - Handle monomial evaluation and constraint generation correctly
 *
 * **Use Cases:**
 * - Non-linear invariant discovery
 * - Polynomial relationship detection (e.g., quadratic forms)
 * - Extending numerical abstract domains to non-linear constraints
 *
 * @param phi The formula to abstract (bit-vector SMT formula)
 * @param variables The set of original variables V = {v_1, ..., v_n}
 * @param monomials The set of template monomials S = {s_1, ..., s_k}
 * @param config Configuration including timeout and max_iterations
 * @return Vector of affine equalities over extended variables (v_1, ..., v_n,
 * s_1, ..., s_k)
 *
 * @note Returns empty vector if variables is empty
 * @note This is a simplified implementation - full Algorithm 13 would require
 *       proper monomial constraint encoding
 */
std::vector<AffineEquality> alpha_poly_V(z3::expr phi,
                                         const std::vector<z3::expr> &variables,
                                         const std::vector<z3::expr> &monomials,
                                         const AbstractionConfig &config) {

  if (variables.empty()) {
    return {};
  }

  context &ctx = phi.ctx();
  size_t n = variables.size();
  size_t k = monomials.size();

  // Extended variable set: (v_1, ..., v_n, s_1, ..., s_k)
  // This treats monomials as additional variables in an affine abstraction
  std::vector<expr> extended_vars = variables;
  extended_vars.insert(extended_vars.end(), monomials.begin(), monomials.end());

  // Initialize [A|b] = [0, ..., 0 | 1]
  std::vector<AffineEquality> A_b;
  AffineEquality initial_row;
  initial_row.variables = extended_vars;
  initial_row.coefficients.resize(n + k, 0);
  initial_row.constant = 1;
  A_b.push_back(initial_row);

  unsigned i = 0;
  unsigned r = 1;

  solver sol(ctx);
  params p(ctx);
  p.set("timeout", config.timeout_ms);
  sol.set(p);

  unsigned iteration = 0;

  // Main loop: while i < r
  while (i < r && iteration < config.max_iterations) {
    if (i >= A_b.size()) {
      break;
    }

    // Extract row (r-i)
    size_t row_idx = r - i - 1;
    const AffineEquality &row = A_b[row_idx];

    // Build disequality: Σ a_j · v_j ≠ b_{r-i}
    // This includes both variables and monomials
    expr sum = ctx.bv_val(0, variables[0].get_sort().bv_size());
    unsigned bv_size = variables[0].get_sort().bv_size();

    for (size_t j = 0; j < row.variables.size() && j < row.coefficients.size();
         ++j) {
      expr var = row.variables[j];
      int64_t coeff = row.coefficients[j];

      if (coeff == 0)
        continue;

      if (var.get_sort().bv_size() != bv_size) {
        var = adjustBitwidth(var, bv_size);
      }

      if (j < n) {
        // Regular variable
        if (coeff == 1) {
          sum = sum + var;
        } else if (coeff == -1) {
          sum = sum - var;
        } else if (coeff > 0) {
          expr coeff_bv = ctx.bv_val(static_cast<uint64_t>(coeff), bv_size);
          sum = sum + (coeff_bv * var);
        } else {
          expr coeff_bv = ctx.bv_val(static_cast<uint64_t>(-coeff), bv_size);
          sum = sum - (coeff_bv * var);
        }
      } else {
        // Monomial variable
        // For monomials, we need to ensure the constraint matches
        // the actual monomial value
        // This is simplified - full implementation would properly
        // encode monomial constraints
      }
    }

    expr b_bv = ctx.bv_val(static_cast<uint64_t>(row.constant), bv_size);
    expr psi = sum != b_bv;

    // Check satisfiability
    sol.push();
    sol.add(phi);
    sol.add(psi);

    if (sol.check() == sat) {
      // Found violating model
      model m = sol.get_model();

      // Extract variable values
      std::vector<int64_t> model_values;
      for (size_t j = 0; j < n; ++j) {
        expr val = m.eval(variables[j], true);
        auto as_int = SymAbs::to_int64(val);
        model_values.push_back(as_int.hasValue() ? as_int.getValue() : 0);
      }

      // Evaluate monomials
      std::vector<int64_t> monomial_values;
      for (const auto &mon : monomials) {
        int64_t val = eval_monomial(mon, m, ctx);
        monomial_values.push_back(val);
      }

      // Create identity system with extended variables
      std::vector<AffineEquality> identity_system;
      for (size_t j = 0; j < n; ++j) {
        AffineEquality eq;
        eq.variables = extended_vars;
        eq.coefficients.resize(n + k, 0);
        eq.coefficients[j] = 1;
        eq.constant = model_values[j];
        identity_system.push_back(eq);
      }

      for (size_t j = 0; j < k; ++j) {
        AffineEquality eq;
        eq.variables = extended_vars;
        eq.coefficients.resize(n + k, 0);
        eq.coefficients[n + j] = 1;
        eq.constant = monomial_values[j];
        identity_system.push_back(eq);
      }

      // Join: [A'|b'] = [A|b] ⊔_aff [Id | (m(v_1), ..., m(v_n), p_1, ...,
      // p_k)^T] Use alpha_aff_V to compute the join properly For polynomial
      // abstraction, we treat it as affine abstraction over extended variables
      // Build formula representing the identity system
      expr identity_formula = ctx.bool_val(true);
      for (size_t j = 0; j < n; ++j) {
        unsigned bv_size = variables[j].get_sort().bv_size();
        identity_formula =
            identity_formula &&
            (variables[j] ==
             ctx.bv_val(static_cast<uint64_t>(model_values[j]), bv_size));
      }
      for (size_t j = 0; j < k; ++j) {
        unsigned bv_size = monomials[j].get_sort().bv_size();
        identity_formula =
            identity_formula &&
            (monomials[j] ==
             ctx.bv_val(static_cast<uint64_t>(monomial_values[j]), bv_size));
      }

      // Compute affine abstraction of current system joined with identity
      // This is a simplified approach - full implementation would use proper
      // matrix operations For now, we add the identity system and triangularize
      A_b.insert(A_b.end(), identity_system.begin(), identity_system.end());

      // Simple triangularization: remove redundant rows
      // Full implementation would use proper Gaussian elimination
      // Remove duplicate rows
      std::vector<AffineEquality> unique_rows;
      for (const auto &row : A_b) {
        bool is_duplicate = false;
        for (const auto &existing : unique_rows) {
          if (row.coefficients == existing.coefficients &&
              row.constant == existing.constant) {
            is_duplicate = true;
            break;
          }
        }
        if (!is_duplicate) {
          unique_rows.push_back(row);
        }
      }
      A_b = unique_rows;

      r = A_b.size();
      i = 0;
    } else {
      ++i;
    }

    sol.pop();
    ++iteration;
  }

  return A_b;
}

} // namespace SymAbs
