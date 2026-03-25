#pragma once

/**
 * @file SymAbsUtils.h
 * @brief Utility functions for symbolic abstraction algorithms
 *
 * This header provides common utility functions used across multiple
 * symbolic abstraction implementations, including:
 * - Bit-vector to integer conversions
 * - Model value extraction
 * - Arithmetic operations (GCD, LCM, floor division)
 * - Rational number arithmetic
 * - Matrix operations (RREF)
 */

#include <cstdint>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <z3++.h>

namespace SymAbs {

/**
 * @brief Convert a signed bit-vector to an unbounded integer expression.
 *
 * Uses an ITE to interpret the bit-vector in two's complement and avoid
 * wrap-around during arithmetic.
 */
z3::expr bv_signed_to_int(const z3::expr &bv);

/**
 * @brief Extract a signed 64-bit integer from a Z3 numeral expression.
 */
llvm::Optional<int64_t> to_int64(const z3::expr &val);

/**
 * @brief Extract integer value from a Z3 model for a given expression.
 *
 * Handles both integer numerals and bit-vectors, converting bit-vectors
 * to signed integers with proper sign extension.
 *
 * @param m The Z3 model
 * @param v The expression to evaluate
 * @param out Output parameter for the extracted value
 * @return true if extraction succeeded, false otherwise
 */
bool eval_model_value(const z3::model &m, const z3::expr &v, int64_t &out);

/**
 * @brief Extract a point (vector of integer values) from a model.
 *
 * @param m The Z3 model
 * @param vars The variables to extract values for
 * @return Vector of integer values corresponding to the variables
 */
std::vector<int64_t> extract_point(const z3::model &m,
                                   const std::vector<z3::expr> &vars);

/**
 * @brief Compute GCD of two 64-bit integers.
 */
int64_t gcd64(int64_t a, int64_t b);

/**
 * @brief Compute LCM of two 64-bit integers.
 */
int64_t lcm64(int64_t a, int64_t b);

/**
 * @brief Compute floor division: ⌊num/denom⌋
 *
 * @param num Numerator
 * @param denom Denominator (must be positive)
 * @return Floor of num/denom
 */
int64_t div_floor(int64_t num, int64_t denom);

/**
 * @brief Simple rational number class for exact arithmetic.
 */
class Rational {
  int64_t num_;
  int64_t den_;

  void normalize();

public:
  Rational();
  explicit Rational(int64_t n);
  Rational(int64_t n, int64_t d);

  int64_t numerator() const { return num_; }
  int64_t denominator() const { return den_; }

  Rational operator+(const Rational &other) const;
  Rational operator-(const Rational &other) const;
  Rational operator*(const Rational &other) const;
  Rational operator/(const Rational &other) const;
  Rational &operator/=(const Rational &other);
  Rational &operator-=(const Rational &other);
  Rational operator-() const;

  bool operator==(const Rational &other) const;
  bool operator!=(const Rational &other) const;
};

/**
 * @brief Result of reduced row echelon form computation.
 */
struct RrefResult {
  std::vector<std::vector<Rational>> matrix;
  std::vector<size_t> pivot_columns;
};

/**
 * @brief Compute reduced row echelon form (RREF) of a matrix over rationals.
 *
 * @param input Input matrix (rows x cols)
 * @return RREF result with normalized matrix and pivot column indices
 */
RrefResult rref(const std::vector<std::vector<Rational>> &input);

} // namespace SymAbs
