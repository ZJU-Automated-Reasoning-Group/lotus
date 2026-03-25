/**
 * @file Zone.cpp
 * @brief Implementation of Zone abstraction (Difference Bound Matrices - DBM)
 *
 * This module implements zone abstraction (also known as Difference Bound
 * Matrices) for computing the least zone describing a set of bit-vectors V
 * subject to formula φ.
 *
 * **Zone Domain:**
 * The zone domain is a relational numerical abstract domain that tracks
 * difference constraints of the form x - y ≤ c and unary constraints of the
 * form x ≤ c. Zones are represented using Difference Bound Matrices (DBM),
 * which efficiently store and manipulate these constraints.
 *
 * **Constraint Forms:**
 * - **Unary constraints**: ⟨v_i⟩ ≤ d  (upper bounds on individual variables)
 * - **Binary constraints**: ⟨v_i⟩ - ⟨v_j⟩ ≤ d  (difference bounds between
 * pairs)
 *
 * **Comparison with Octagon Domain:**
 * - Zones are **more restrictive** than octagons (octagons allow ±x ± y ≤ c)
 * - Zones are **computationally more efficient** (O(n²) vs O(n²) but with
 * simpler constraint structure and faster closure operations)
 * - Zones are **sufficient** for many applications, particularly:
 *   * Timing analysis in real-time systems
 *   * Scheduling problems
 *   * Verification of systems with deadline constraints
 *
 * **Mathematical Properties:**
 * Zones form a lattice with meet (conjunction), join (disjunction), and closure
 * (transitive closure of difference constraints) operations. The closure
 * operation (using Floyd-Warshall algorithm) ensures consistency and canonical
 * form.
 *
 * **Algorithm Overview:**
 * The algorithm computes bounds for all unary and binary constraints:
 * 1. For each variable v_i, compute upper bound using α_lin-exp with expression
 * v_i
 * 2. For each pair (v_i, v_j), compute bound for difference v_i - v_j using
 * α_lin-exp
 * 3. Collect all valid constraints into the result
 *
 * **Use Cases:**
 * - Real-time system verification
 * - Timing analysis and deadline checking
 * - Scheduling constraint verification
 * - Any analysis where difference constraints are sufficient
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <cassert>

#include <llvm/ADT/Optional.h>
#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {

/**
 * @brief Convert a zone constraint to a Z3 integer inequality expression.
 *
 * This function converts a ZoneConstraint structure into its corresponding Z3
 * expression. Zone constraints can be either unary (single variable upper
 * bound) or binary (difference constraint between two variables).
 *
 * **Constraint Forms:**
 * - Unary: ⟨v_i⟩ ≤ d  (when c.unary == true)
 * - Binary: ⟨v_i⟩ - ⟨v_j⟩ ≤ d  (when c.unary == false)
 *
 * @param c The zone constraint to convert
 * @return Z3 expression representing the constraint as an integer inequality
 */
z3::expr zone_constraint_to_expr(const ZoneConstraint &c) {
  context &ctx = c.var_i.ctx();

  if (c.unary) {
    // Unary constraint: v_i ≤ d (upper bound on variable v_i)
    return SymAbs::bv_signed_to_int(c.var_i) <= ctx.int_val(c.bound);
  } else {
    // Binary constraint: v_i - v_j ≤ d (difference bound)
    expr lhs =
        SymAbs::bv_signed_to_int(c.var_i) - SymAbs::bv_signed_to_int(c.var_j);
    return lhs <= ctx.int_val(c.bound);
  }
}

/**
 * @brief Compute zone abstraction α_zone^V(φ)
 *
 * Computes the least zone (Difference Bound Matrix) describing the set of
 * bit-vectors V subject to formula φ. The algorithm systematically computes
 * bounds for all unary and binary difference constraints.
 *
 * **Algorithm Steps:**
 * 1. **Unary constraints**: For each variable v_i, compute the maximum value
 *    using α_lin-exp with expression v_i, giving upper bound v_i ≤ d
 *
 * 2. **Binary constraints**: For each pair (v_i, v_j) with i ≠ j, compute
 *    the maximum of v_i - v_j using α_lin-exp, giving difference bound
 *    v_i - v_j ≤ d
 *
 * 3. Collect all valid constraints (where bounds can be computed) into the
 * result
 *
 * **Note on Lower Bounds:**
 * The current implementation computes upper bounds for unary constraints (v_i ≤
 * d). Lower bounds can be obtained by computing -v_i ≤ d' and deriving v_i ≥
 * -d', but in standard zone representation, lower bounds are typically
 * expressed as difference constraints relative to a special zero variable, or
 * as unary constraints using negation. The code includes a commented section
 * for lower bounds that can be extended if needed.
 *
 * **Example:**
 * For variables {x, y} and formula φ = (x ≥ 0) ∧ (x ≤ 10) ∧ (y ≥ x) ∧ (y ≤ 20),
 * the algorithm would produce constraints like:
 * - x ≤ 10, y ≤ 20 (unary upper bounds)
 * - y - x ≤ 0 (from y ≥ x, which gives y - x ≤ 0)
 * - x - y ≤ 10 (from the combined bounds)
 *
 * @param phi The formula constraining the variables (bit-vector SMT formula)
 * @param variables The set of bit-vector variables V = {v_1, ..., v_n}
 * @param config Configuration including timeout and iteration limits
 * @return Vector of zone constraints representing the least zone
 *
 * @note The algorithm has O(n²) complexity where n is the number of variables,
 *       as it considers all pairs of variables for difference constraints.
 * @note Constraints that cannot be bounded (α_lin-exp returns None) are
 * skipped.
 * @note Reflexive constraints (v_i - v_i ≤ 0) are always true and are skipped.
 */
std::vector<ZoneConstraint> alpha_zone_V(z3::expr phi,
                                         const std::vector<z3::expr> &variables,
                                         const AbstractionConfig &config) {

  std::vector<ZoneConstraint> constraints;

  if (variables.empty()) {
    return constraints;
  }

  context &ctx = phi.ctx();

  // Step 1: Compute unary constraints (upper bounds): v_i ≤ d
  // For each variable, find its maximum value under φ
  for (size_t i = 0; i < variables.size(); ++i) {
    LinearExpression lexpr;
    lexpr.variables.push_back(variables[i]);
    lexpr.coefficients.push_back(1);

    // Compute maximum value of v_i using Algorithm 7
    auto bound = alpha_lin_exp(phi, lexpr, config);
    if (bound.hasValue()) {
      constraints.emplace_back(variables[i], bound.getValue());
    }

    // Note: Lower bounds can be computed similarly by maximizing -v_i
    // The commented code below shows how to compute lower bounds:
    // LinearExpression lexpr_neg;
    // lexpr_neg.variables.push_back(variables[i]);
    // lexpr_neg.coefficients.push_back(-1);
    // auto bound_neg = alpha_lin_exp(phi, lexpr_neg, config);
    // If needed, lower bounds can be stored or converted to difference
    // constraints
  }

  // Step 2: Compute binary constraints (difference bounds): v_i - v_j ≤ d
  // For each pair of distinct variables, find the maximum difference
  for (size_t i = 0; i < variables.size(); ++i) {
    for (size_t j = 0; j < variables.size(); ++j) {
      if (i == j) {
        // Skip reflexive constraints: v_i - v_i ≤ 0 is always true
        continue;
      }

      // Construct linear expression: v_i - v_j
      LinearExpression lexpr;
      lexpr.variables.push_back(variables[i]);
      lexpr.coefficients.push_back(1);
      lexpr.variables.push_back(variables[j]);
      lexpr.coefficients.push_back(-1);

      // Compute maximum of v_i - v_j using Algorithm 7
      auto bound = alpha_lin_exp(phi, lexpr, config);
      if (bound.hasValue()) {
        constraints.emplace_back(variables[i], variables[j], bound.getValue());
      }
    }
  }

  return constraints;
}

} // namespace SymAbs
