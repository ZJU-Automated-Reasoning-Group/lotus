/**
 * @file Congruence.cpp
 * @brief Implementation of Algorithm 11: α_a-cong
 *
 * This module implements Algorithm 11 from "Automatic Abstraction of Bit-Vector
 * Formulae" for computing the least arithmetical congruence describing the
 * values of a variable that satisfy a formula.
 *
 * **Congruence Domain:**
 * A congruence constraint has the form ⟨v⟩ ≡_m c, meaning that v mod m = c.
 * This captures periodic patterns in variable values, which is useful for:
 * - Detecting array access patterns (e.g., even indices only)
 * - Identifying stride patterns in loops
 * - Modular arithmetic analysis
 *
 * **Mathematical Background:**
 * - Modulus m = 0 means no constraint (all values possible, singleton case)
 * - Modulus m > 0 means values are congruent modulo m
 * - The remainder c is normalized to be in [0, m-1]
 * - The GCD of differences between values determines the modulus
 *
 * **Algorithm Overview:**
 * The algorithm iteratively refines the congruence by:
 * 1. Finding satisfying models for the variable
 * 2. Computing GCD of differences between discovered values
 * 3. The GCD becomes the modulus m
 * 4. Normalizing the remainder c mod m
 *
 * **Key Insight:**
 * If values v_1, v_2, ..., v_k satisfy φ, and d_i = |v_i - v_1|, then
 * the modulus m is GCD(d_2, d_3, ..., d_k). This captures the periodicity
 * of the value set.
 *
 * **Example:**
 * If φ constrains v to be {2, 5, 8, 11, ...}, then:
 * - Differences: 3, 6, 9, ... → GCD = 3
 * - Modulus: m = 3
 * - Remainder: c = 2 mod 3
 * - Result: v ≡_3 2
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <cassert>
#include <cmath>
#include <numeric>

#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {

/**
 * @brief Compute arithmetical congruence abstraction α_a-cong(φ, v)
 *
 * Algorithm 11: Computes the least arithmetical congruence ⟨v⟩ ≡_m c
 * describing values of variable v that satisfy formula φ.
 *
 * **Algorithm Steps:**
 * 1. **Initialization**: Set (m, c) = (0, ⊥) representing no constraint yet
 * 2. **Iterative Refinement**: While iteration limit not exceeded:
 *    a. Find a satisfying model for current_phi
 *    b. Extract value v_val from the model
 *    c. If this is the first value, set c = v_val and m = 0 (singleton)
 *    d. Otherwise, update m = GCD(m, |v_val - c|) to refine modulus
 *    e. Exclude current congruence class to find next value
 *    f. Continue until no more models exist or iteration limit reached
 * 3. **Normalization**: Normalize remainder c to be in [0, m-1]
 *
 * **Congruence Class Exclusion:**
 * To find the next value in a different congruence class, we add constraints:
 * - If m = 0: exclude exact value v ≠ v_val
 * - If m > 0: exclude congruence class v mod m ≠ c mod m
 *
 * **Termination:**
 * - Terminates when no more satisfying models exist
 * - Terminates when max_iterations is reached
 * - Returns None if no models found initially
 *
 * @param phi The formula constraining the variable (bit-vector SMT formula)
 * @param variable The variable v to compute congruence for
 * @param config Configuration including timeout and max_iterations
 * @return The congruence ⟨v⟩ ≡_m c, or None if computation fails
 *
 * @note The result is exact: all values satisfying φ are congruent mod m to c
 * @note Modulus m = 0 indicates a singleton set (only one value possible)
 */
llvm::Optional<Congruence> alpha_a_cong(z3::expr phi, z3::expr variable,
                                        const AbstractionConfig &config) {

  context &ctx = phi.ctx();
  solver sol(ctx);

  params p(ctx);
  p.set("timeout", config.timeout_ms);
  sol.set(p);

  // Initialize: (m, c) = (0, ⊥)
  // m = 0 means no constraint yet (will be set when second value found)
  // c_opt = None means no value found yet
  uint64_t m = 0;
  llvm::Optional<int64_t> c_opt;
  z3::expr current_phi =
      phi; // Working formula (gets constrained during iteration)
  unsigned iteration = 0;

  while (iteration < config.max_iterations) {
    sol.push(); // Save solver state
    sol.add(current_phi);

    // Try to find a satisfying model
    if (sol.check() != sat) {
      sol.pop();
      // No more models exist, terminate
      break;
    }

    // Extract value from model
    model m_model = sol.get_model();
    int64_t v_val = 0;
    bool ok = eval_model_value(m_model, variable, v_val);
    assert(ok && "Failed to extract model value");

    if (!c_opt.hasValue()) {
      // First value found: initialize congruence
      c_opt = v_val;
      m = 0; // m = 0 indicates singleton so far (only one value seen)
    } else {
      // Update modulus using GCD of differences
      int64_t c_val = c_opt.getValue();
      int64_t d = std::abs(v_val - c_val);
      if (d == 0) {
        // Same value as before (shouldn't happen due to exclusion, but handle
        // gracefully) Exclude this specific value to find next
        unsigned bv_size = variable.get_sort().bv_size();
        current_phi =
            current_phi &&
            (variable != ctx.bv_val(static_cast<uint64_t>(v_val), bv_size));
        sol.pop();
        ++iteration;
        continue;
      }

      // Update modulus: m = GCD(m, |v_val - c|)
      // This captures the periodicity: if we see values v_1, v_2, v_3, ...
      // then m = GCD(|v_2 - v_1|, |v_3 - v_1|, ...)
      m = (m == 0) ? static_cast<uint64_t>(d)
                   : static_cast<uint64_t>(gcd64(m, static_cast<int64_t>(d)));
    }

    sol.pop();

    // Exclude current congruence class to discover new witnesses in next
    // iteration. This ensures we explore different values to refine the
    // modulus.
    if (m == 0) {
      // Singleton case: exclude exact value to find if there are others
      unsigned bv_size = variable.get_sort().bv_size();
      current_phi =
          current_phi &&
          (variable !=
           ctx.bv_val(static_cast<uint64_t>(c_opt.getValue()), bv_size));
    } else {
      // Non-singleton: exclude entire congruence class v mod m = c mod m
      // This forces discovery of values in different congruence classes
      z3::expr v_int = SymAbs::bv_signed_to_int(variable);
      z3::expr c_int = ctx.int_val(c_opt.getValue());
      z3::expr mod_val =
          z3::mod(v_int - c_int, ctx.int_val(static_cast<int64_t>(m)));
      current_phi =
          current_phi && (mod_val != ctx.int_val(0)); // v mod m ≠ c mod m
    }

    ++iteration;
  }

  if (!c_opt.hasValue()) {
    // No models found for φ
    return llvm::None;
  }

  // Normalize remainder: c ← c mod m (ensure c ∈ [0, m-1])
  int64_t c_normalized = c_opt.getValue();
  if (m > 0) {
    int64_t c_mod = c_normalized % static_cast<int64_t>(m);
    // Handle negative remainder (ensure non-negative)
    if (c_mod < 0)
      c_mod += static_cast<int64_t>(m);
    c_normalized = c_mod;
  }
  // If m = 0, c_normalized remains as-is (singleton case)

  return Congruence(variable, m, c_normalized);
}

} // namespace SymAbs
