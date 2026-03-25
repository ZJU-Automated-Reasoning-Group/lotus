#pragma once

/**
 * @file Congruence.h
 * @brief Arithmetical congruence abstraction (Algorithm 11: α_a-cong)
 */

#include "Solvers/SMT/SymAbs/Config.h"

#include <cstdint>

#include <llvm/ADT/Optional.h>
#include <z3++.h>

namespace SymAbs {

/**
 * @brief Represents a congruence: ⟨v⟩ ≡_m c
 */
struct Congruence {
  z3::expr variable;
  uint64_t modulus;  // m
  int64_t remainder; // c

  Congruence(z3::expr v, uint64_t m, int64_t r)
      : variable(v), modulus(m), remainder(r) {}
};

/**
 * @brief Compute arithmetical congruence abstraction α_a-cong(φ)
 *
 * Algorithm 11: Computes the least arithmetical congruence ⟨v⟩ ≡_m c
 * describing values of v that satisfy φ.
 *
 * @param phi The formula
 * @param variable The variable v to abstract
 * @param config Configuration
 * @return The congruence, or llvm::None on failure
 */
llvm::Optional<Congruence>
alpha_a_cong(z3::expr phi, z3::expr variable,
             const AbstractionConfig &config = AbstractionConfig{});

} // namespace SymAbs
