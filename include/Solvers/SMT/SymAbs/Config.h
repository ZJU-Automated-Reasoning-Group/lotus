#pragma once

/**
 * @file Config.h
 * @brief Configuration types for symbolic abstraction algorithms
 */

namespace SymAbs {

/**
 * @brief Result of a symbolic abstraction operation
 */
enum class AbstractionResult { Success, Timeout, Error, Unsatisfiable };

/**
 * @brief Configuration for abstraction algorithms
 */
struct AbstractionConfig {
  unsigned timeout_ms = 10000;    // Timeout in milliseconds
  bool verbose = false;           // Enable verbose output
  unsigned max_iterations = 1000; // Maximum iterations for iterative algorithms

  // For polyhedral abstraction
  unsigned max_inequalities =
      16; // Stop polyhedral abstraction after this many inequalities

  // For extrapolation
  bool enable_extrapolation = false;
  unsigned small_bitwidth = 4;  // Small bit-width for extrapolation
  unsigned large_bitwidth = 32; // Large bit-width to extrapolate to
};

} // namespace SymAbs
