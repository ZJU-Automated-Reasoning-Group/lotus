#pragma once

/**
 * @file Octagon.h
 * @brief Octagonal abstraction (Algorithm 8: α_oct^V)
 */

#include "Solvers/SMT/SymAbs/Config.h"
#include "Solvers/SMT/SymAbs/LinearExpression.h"

#include <cstdint>
#include <vector>

#include <z3++.h>

namespace SymAbs {

/**
 * @brief Represents an octagonal constraint: ±⟨v_i⟩ ± ⟨v_j⟩ ≤ d
 */
struct OctagonalConstraint {
  z3::expr var_i;
  z3::expr var_j;
  int lambda_i;  // ±1
  int lambda_j;  // ±1
  int64_t bound; // d
  bool unary;    // true when constraint only involves var_i

  OctagonalConstraint(z3::expr v, int lambda, int64_t d)
      : var_i(v), var_j(v), lambda_i(lambda), lambda_j(0), bound(d),
        unary(true) {}

  OctagonalConstraint(z3::expr vi, z3::expr vj, int li, int lj, int64_t d)
      : var_i(vi), var_j(vj), lambda_i(li), lambda_j(lj), bound(d),
        unary(false) {}
};

/**
 * @brief Compute octagonal abstraction α_oct^V(φ)
 *
 * Algorithm 8: Computes the least octagon describing the set of bit-vectors V
 * subject to formula φ.
 *
 * @param phi The formula
 * @param variables The set of bit-vector variables V = {v_1, ..., v_n}
 * @param config Configuration
 * @return Vector of octagonal constraints
 */
std::vector<OctagonalConstraint>
alpha_oct_V(z3::expr phi, const std::vector<z3::expr> &variables,
            const AbstractionConfig &config = AbstractionConfig{});

/**
 * @brief Convert an octagonal constraint to a Z3 integer inequality.
 */
z3::expr oct_constraint_to_expr(const OctagonalConstraint &c);

} // namespace SymAbs
