#pragma once

/**
 * @file LinearExpression.h
 * @brief Linear expression maximization (Algorithm 7: α_lin-exp)
 */

#include "Solvers/SMT/SymAbs/Config.h"

#include <cstdint>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <z3++.h>

namespace SymAbs {

/**
 * @brief Linear expression: Σ_{i=1}^n λ_i · ⟨⟨v_i⟩⟩
 */
struct LinearExpression {
  std::vector<z3::expr> variables;   // v_1, ..., v_n
  std::vector<int64_t> coefficients; // λ_1, ..., λ_n

  LinearExpression() = default;
  LinearExpression(const std::vector<z3::expr> &vars,
                   const std::vector<int64_t> &coeffs)
      : variables(vars), coefficients(coeffs) {
    assert(vars.size() == coeffs.size());
  }
};

/**
 * @brief Compute the least upper bound of a linear expression subject to a
 * formula
 *
 * Algorithm 7: α_lin-exp^V(φ, Σ λ_i · ⟨⟨v_i⟩⟩)
 *
 * @param phi The formula constraining the variables
 * @param expr The linear expression to maximize
 * @param config Configuration for the algorithm
 * @return The least upper bound d ∈ Z, or llvm::None on failure
 */
llvm::Optional<int64_t>
alpha_lin_exp(const z3::expr &phi, const LinearExpression &expr,
              const AbstractionConfig &config = AbstractionConfig{});

/**
 * @brief Compute minimum value (using negation of linear expression)
 */
llvm::Optional<int64_t>
minimum(const z3::expr &phi, const z3::expr &variable,
        const AbstractionConfig &config = AbstractionConfig{});

/**
 * @brief Compute maximum value
 */
llvm::Optional<int64_t>
maximum(const z3::expr &phi, const z3::expr &variable,
        const AbstractionConfig &config = AbstractionConfig{});

} // namespace SymAbs
