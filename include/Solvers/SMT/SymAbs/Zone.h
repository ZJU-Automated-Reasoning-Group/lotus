#pragma once

/**
 * @file Zone.h
 * @brief Zone abstraction (Difference Bound Matrices)
 *
 * The zone domain, also known as Difference Bound Matrices (DBM), is a
 * numerical abstract domain that tracks difference constraints of the form: x -
 * y ≤ c
 *
 * This is more restrictive than the octagon domain which allows ±x ± y ≤ c.
 * Zones are particularly useful for modeling timing constraints and are widely
 * used in real-time system verification.
 */

#include "Solvers/SMT/SymAbs/Config.h"
#include "Solvers/SMT/SymAbs/LinearExpression.h"

#include <cstdint>
#include <vector>

#include <z3++.h>

namespace SymAbs {

/**
 * @brief Represents a zone constraint: ⟨v_i⟩ - ⟨v_j⟩ ≤ d or ⟨v_i⟩ ≤ d
 *
 * Zone constraints are difference constraints of the form:
 * - Binary: x - y ≤ c
 * - Unary: x ≤ c (represented as x - 0 ≤ c)
 */
struct ZoneConstraint {
  z3::expr var_i; // First variable
  z3::expr var_j; // Second variable (for binary constraints)
  int64_t bound;  // The bound d
  bool unary;     // true when constraint only involves var_i

  // Unary constraint: v_i ≤ d
  ZoneConstraint(z3::expr v, int64_t d)
      : var_i(v), var_j(v), bound(d), unary(true) {}

  // Binary constraint: v_i - v_j ≤ d
  ZoneConstraint(z3::expr vi, z3::expr vj, int64_t d)
      : var_i(vi), var_j(vj), bound(d), unary(false) {}
};

/**
 * @brief Compute zone abstraction α_zone^V(φ)
 *
 * Computes the least zone (DBM) describing the set of bit-vectors V
 * subject to formula φ. The algorithm finds bounds for all difference
 * constraints v_i - v_j ≤ d and unary constraints v_i ≤ d.
 *
 * @param phi The formula
 * @param variables The set of bit-vector variables V = {v_1, ..., v_n}
 * @param config Configuration
 * @return Vector of zone constraints
 */
std::vector<ZoneConstraint>
alpha_zone_V(z3::expr phi, const std::vector<z3::expr> &variables,
             const AbstractionConfig &config = AbstractionConfig{});

/**
 * @brief Convert a zone constraint to a Z3 integer inequality.
 *
 * Converts a ZoneConstraint to its corresponding Z3 expression:
 * - Unary: ⟨v_i⟩ ≤ d
 * - Binary: ⟨v_i⟩ - ⟨v_j⟩ ≤ d
 */
z3::expr zone_constraint_to_expr(const ZoneConstraint &c);

} // namespace SymAbs
