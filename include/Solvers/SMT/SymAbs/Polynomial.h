#pragma once

/**
 * @file Polynomial.h
 * @brief Polynomial abstraction (Algorithm 13: α_poly^V)
 */

#include "Solvers/SMT/SymAbs/Affine.h"
#include "Solvers/SMT/SymAbs/Config.h"

#include <vector>

#include <z3++.h>

namespace SymAbs {

/**
 * @brief Compute polynomial abstraction α_poly^V(φ, S)
 *
 * Algorithm 13: Computes polynomial hull with template monomials S.
 *
 * @param phi The formula
 * @param variables The set of variables V
 * @param monomials The set of monomials S = {s_1, ..., s_k}
 * @param config Configuration
 * @return Affine system over extended variables (v_1, ..., v_n, s_1, ..., s_k)
 */
std::vector<AffineEquality>
alpha_poly_V(z3::expr phi, const std::vector<z3::expr> &variables,
             const std::vector<z3::expr> &monomials,
             const AbstractionConfig &config = AbstractionConfig{});

} // namespace SymAbs
