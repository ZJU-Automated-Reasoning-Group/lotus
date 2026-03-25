#pragma once

/**
 * @file Affine.h
 * @brief Affine equality abstraction (Algorithm 12: α_aff^V)
 */

#include "Solvers/SMT/SymAbs/Config.h"

#include <cstdint>
#include <vector>

#include <z3++.h>

namespace SymAbs {

/**
 * @brief Represents an affine equality: Σ c_i · v_i = d
 */
struct AffineEquality {
  std::vector<z3::expr> variables;
  std::vector<int64_t> coefficients;
  int64_t constant;

  AffineEquality() : constant(0) {}
};

/**
 * @brief Compute affine equality abstraction α_aff^V(φ)
 *
 * Algorithm 12: Computes the affine hull of models of φ.
 *
 * @param phi The formula
 * @param variables The set of variables V
 * @param config Configuration
 * @return Affine system [A|b] represented as a vector of affine equalities
 */
std::vector<AffineEquality>
alpha_aff_V(z3::expr phi, const std::vector<z3::expr> &variables,
            const AbstractionConfig &config = AbstractionConfig{});

} // namespace SymAbs
