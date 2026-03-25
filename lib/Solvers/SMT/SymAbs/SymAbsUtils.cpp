/**
 * @file SymAbsUtils.cpp
 * @brief Implementation of utility functions for symbolic abstraction
 *
 * This module provides utility functions used across multiple symbolic
 * abstraction algorithms. These functions handle common operations like
 * bit-vector to integer conversion, model value extraction, arithmetic
 * operations, and matrix computations.
 *
 * **Key Utilities:**
 * - Bit-vector conversion: Converts bit-vectors to unbounded integers (two's
 * complement)
 * - Model extraction: Extracts integer values from Z3 models
 * - Arithmetic: GCD, LCM, floor division
 * - Rational numbers: Exact rational arithmetic for matrix operations
 * - Matrix operations: Reduced row echelon form (RREF) for linear algebra
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"

#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <numeric>

#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {

// ============================================================================
// Bit-vector to integer conversion
// ============================================================================

/**
 * @brief Convert a signed bit-vector to an unbounded integer expression.
 *
 * This function interprets a bit-vector in two's complement representation
 * and converts it to an unbounded integer. This is crucial for symbolic
 * abstraction algorithms because:
 * - Bit-vector arithmetic has wrap-around (modular arithmetic)
 * - Integer arithmetic is unbounded and easier to reason about
 * - Two's complement interpretation preserves signed value semantics
 *
 * **Conversion Method:**
 * - Extract the most significant bit (MSB) as the sign bit
 * - Convert bit-vector to unsigned integer using Z3's bv2int
 * - If MSB = 1 (negative), subtract 2^w to get two's complement value
 * - If MSB = 0 (positive), use unsigned value directly
 *
 * **Example:**
 * - 4-bit bit-vector 0b1111 (unsigned: 15) → -1 (since MSB=1, 15 - 16 = -1)
 * - 4-bit bit-vector 0b0111 (unsigned: 7) → 7 (since MSB=0)
 *
 * @param bv The bit-vector expression to convert
 * @return Z3 integer expression representing the two's complement value as
 * unbounded integer
 *
 * @note The result is an unbounded integer, avoiding wrap-around issues
 * @note This conversion is used throughout symbolic abstraction to work with
 *       unbounded integer arithmetic instead of modular bit-vector arithmetic
 */
expr bv_signed_to_int(const expr &bv) {
  context &ctx = bv.ctx();
  // Let Z3 perform signed two's-complement conversion directly.
  return to_expr(ctx, Z3_mk_bv2int(ctx, bv, true));
}

// ============================================================================
// Integer extraction from Z3 expressions
// ============================================================================

/**
 * @brief Extract a signed 64-bit integer from a Z3 numeral expression.
 *
 * Attempts to extract an integer value from a Z3 expression that represents
 * a numeral (constant). Uses Z3's native extraction first, with string-based
 * fallback for values that don't fit in 64 bits or when native extraction
 * fails.
 *
 * @param val The Z3 expression (must be a numeral)
 * @return The integer value, or None if extraction fails or expression is not a
 * numeral
 *
 * @note Returns None if val is not a numeral expression
 * @note May fail for very large integers that exceed int64_t range
 */
llvm::Optional<int64_t> to_int64(const expr &val) {
  if (!val.is_numeral()) {
    return llvm::None;
  }
  int64_t out = 0;
  // Try Z3's native extraction first (efficient)
  if (Z3_get_numeral_int64(val.ctx(), val, &out)) {
    return out;
  }
  // Fallback through string conversion if the helper fails (e.g., big values)
  // This handles cases where the value doesn't fit in int64_t directly
  try {
    std::string s = Z3_get_numeral_string(val.ctx(), val);
    return std::stoll(s);
  } catch (...) {
    return llvm::None;
  }
}

/**
 * @brief Extract integer value from a Z3 model for a given expression.
 *
 * Evaluates an expression under a model and extracts its integer value.
 * Handles both integer numerals and bit-vectors, converting bit-vectors
 * to signed integers with proper sign extension.
 *
 * **Bit-vector Handling:**
 * For bit-vectors, the function:
 * - Extracts the unsigned value
 * - Performs sign extension based on the most significant bit
 * - Handles bit-widths up to 64 bits (larger widths return failure)
 *
 * @param m The Z3 model providing variable assignments
 * @param v The expression to evaluate
 * @param out Output parameter for the extracted value
 * @return true if extraction succeeded, false otherwise
 *
 * @note Returns false if expression cannot be evaluated or converted to integer
 * @note Bit-vectors larger than 64 bits cannot be extracted
 */
bool eval_model_value(const model &m, const expr &v, int64_t &out) {
  expr val = m.eval(v, true);
  if (val.is_bv()) {
    unsigned bv_size = val.get_sort().bv_size();
    if (bv_size > 64) {
      return false;
    }
    uint64_t unsigned_val = 0;
    if (!Z3_get_numeral_uint64(val.ctx(), val, &unsigned_val)) {
      return false;
    }
    int64_t signed_val = static_cast<int64_t>(unsigned_val);
    if (bv_size < 64) {
      const int64_t mask = (1LL << bv_size) - 1;
      const int64_t sign_mask = 1LL << (bv_size - 1);
      if (signed_val & sign_mask) {
        signed_val |= ~mask;
      }
    }
    out = signed_val;
    return true;
  }

  if (val.is_numeral()) {
    std::string num_str = Z3_get_numeral_string(val.ctx(), val);
    try {
      out = std::stoll(num_str);
      return true;
    } catch (...) {
      return false;
    }
  }

  return false;
}

/**
 * @brief Extract a point (vector of integer values) from a model.
 *
 * Extracts integer values for a set of variables from a Z3 model, producing
 * a point in n-dimensional space where n is the number of variables.
 *
 * **Use Case:**
 * This function is used extensively in polyhedral and affine abstraction
 * algorithms to extract concrete points from models for convex hull
 * computation.
 *
 * @param m The Z3 model
 * @param vars The variables to extract values for
 * @return Vector of integer values corresponding to the variables (point
 * coordinates)
 *
 * @note Asserts if value extraction fails (should not happen for valid models)
 * @note Returns 0 as fallback value if extraction fails (should not occur)
 */
std::vector<int64_t> extract_point(const model &m,
                                   const std::vector<expr> &vars) {
  std::vector<int64_t> point;
  point.reserve(vars.size());
  for (const auto &v : vars) {
    int64_t val = 0;
    bool ok = eval_model_value(m, v, val);
    assert(ok && "Failed to extract model value");
    if (!ok) {
      val = 0; // Fallback (should not occur)
    }
    point.push_back(val);
  }
  return point;
}

// ============================================================================
// Arithmetic utilities
// ============================================================================

/**
 * @brief Compute greatest common divisor (GCD) of two 64-bit integers.
 *
 * Uses Euclid's algorithm to compute GCD. The result is always non-negative.
 * This function is used extensively in:
 * - Congruence abstraction (Algorithm 11) to compute modulus
 * - Rational number normalization
 * - Constraint normalization (GCD of coefficients)
 *
 * @param a First integer
 * @param b Second integer
 * @return GCD(|a|, |b|) (always non-negative)
 */
int64_t gcd64(int64_t a, int64_t b) {
  auto abs_u64 = [](int64_t x) -> uint64_t {
    if (x >= 0) {
      return static_cast<uint64_t>(x);
    }
    if (x == std::numeric_limits<int64_t>::min()) {
      return (1ULL << 63);
    }
    return static_cast<uint64_t>(-x);
  };
  uint64_t ua = abs_u64(a);
  uint64_t ub = abs_u64(b);
  // Euclidean algorithm
  while (ub != 0) {
    uint64_t temp = ub;
    ub = ua % ub;
    ua = temp;
  }
  if (ua > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  return static_cast<int64_t>(ua);
}

/**
 * @brief Compute least common multiple (LCM) of two 64-bit integers.
 *
 * Computes LCM using the relationship: LCM(a, b) = |a * b| / GCD(a, b).
 * Returns 0 if either input is 0.
 *
 * **Use Case:**
 * Used in matrix operations to scale rational coefficients to integers,
 * particularly in facet computation and constraint normalization.
 *
 * @param a First integer
 * @param b Second integer
 * @return LCM(|a|, |b|) if both are non-zero, 0 otherwise
 */
int64_t lcm64(int64_t a, int64_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  // LCM(a, b) = |a * b| / GCD(a, b)
  // Note: Division before multiplication to avoid overflow
  return std::abs(a / gcd64(a, b) * b);
}

/**
 * @brief Compute floor division: ⌊num/denom⌋
 *
 * Computes the floor (round toward negative infinity) of the division.
 * This is different from standard integer division in C++ for negative numbers:
 * - Standard: -5 / 2 = -2 (truncates toward zero)
 * - Floor: ⌊-5/2⌋ = -3 (rounds down)
 *
 * **Use Case:**
 * Used in octagonal abstraction normalization where 2·v ≤ d needs to be
 * normalized to v ≤ ⌊d/2⌋, ensuring soundness (floor preserves the constraint).
 *
 * @param num Numerator
 * @param denom Denominator (must be positive)
 * @return Floor of num/denom
 *
 * @pre denom > 0 (asserted if violated)
 */
int64_t div_floor(int64_t num, int64_t denom) {
  assert(denom > 0 && "denominator must be positive");
  int64_t q = num / denom;
  int64_t r = num % denom;
  if (r != 0 && num < 0) {
    --q;
  }
  return q;
}

// ============================================================================
// Rational number class
// ============================================================================

/**
 * @brief Normalize a rational number to canonical form.
 *
 * Normalizes a rational number by:
 * 1. Handling division by zero (sets to 0/1)
 * 2. Ensuring denominator is positive (flips sign of both if needed)
 * 3. Reducing by GCD to get irreducible form
 *
 * **Canonical Form:**
 * - Denominator is always positive
 * - Numerator and denominator are coprime (GCD = 1)
 * - Zero is represented as 0/1
 *
 * This normalization ensures unique representation and prevents denominator
 * growth during arithmetic operations.
 */
void Rational::normalize() {
  if (den_ == 0) {
    // Division by zero: represent as 0/1
    num_ = 0;
    den_ = 1;
    return;
  }
  // Ensure denominator is positive (flip sign if needed)
  if (den_ < 0) {
    num_ = -num_;
    den_ = -den_;
  }
  // Reduce by GCD to get irreducible form
  int64_t g = gcd64(num_, den_);
  if (g > 1) {
    num_ /= g;
    den_ /= g;
  }
}

Rational::Rational() : num_(0), den_(1) {}

Rational::Rational(int64_t n) : num_(n), den_(1) {}

Rational::Rational(int64_t n, int64_t d) : num_(n), den_(d) { normalize(); }

Rational Rational::operator+(const Rational &other) const {
  int64_t n = num_ * other.den_ + other.num_ * den_;
  int64_t d = den_ * other.den_;
  return Rational(n, d);
}

Rational Rational::operator-(const Rational &other) const {
  int64_t n = num_ * other.den_ - other.num_ * den_;
  int64_t d = den_ * other.den_;
  return Rational(n, d);
}

Rational Rational::operator*(const Rational &other) const {
  int64_t n = num_ * other.num_;
  int64_t d = den_ * other.den_;
  return Rational(n, d);
}

Rational Rational::operator/(const Rational &other) const {
  int64_t n = num_ * other.den_;
  int64_t d = den_ * other.num_;
  return Rational(n, d);
}

Rational &Rational::operator/=(const Rational &other) {
  *this = *this / other;
  return *this;
}

Rational &Rational::operator-=(const Rational &other) {
  *this = *this - other;
  return *this;
}

Rational Rational::operator-() const { return Rational(-num_, den_); }

bool Rational::operator==(const Rational &other) const {
  return num_ == other.num_ && den_ == other.den_;
}

bool Rational::operator!=(const Rational &other) const {
  return !(*this == other);
}

// ============================================================================
// Matrix operations
// ============================================================================

/**
 * @brief Compute reduced row echelon form (RREF) of a matrix over rationals.
 *
 * This function implements Gaussian elimination with exact rational arithmetic
 * to compute the RREF of a matrix. The RREF is used extensively in:
 * - Affine abstraction (Algorithm 12) to find nullspace basis
 * - Polyhedral abstraction (Algorithm 9) for facet computation
 * - Computing linear dependencies between vectors
 *
 * **RREF Properties:**
 * - Leading entry (pivot) in each row is 1
 * - Each pivot is to the right of pivots in rows above
 * - All entries above and below pivots are 0
 * - Zero rows are at the bottom
 *
 * **Algorithm (Gaussian Elimination):**
 * 1. For each column c from left to right:
 *    a. Find first non-zero entry (pivot) in column c
 *    b. Swap pivot row to current position
 *    c. Normalize pivot row (divide by pivot value)
 *    d. Eliminate entries in column c for all other rows
 * 2. Return normalized matrix and pivot column indices
 *
 * **Output:**
 * - matrix: The RREF matrix (in-place modification)
 * - pivot_columns: Indices of columns containing pivots
 *
 * @param input Input matrix (rows x cols) over rational numbers
 * @return RrefResult containing the RREF matrix and pivot column indices
 *
 * @note Returns empty result if input is empty
 * @note Uses exact rational arithmetic to avoid floating-point precision issues
 * @note Time complexity: O(rows × cols × min(rows, cols))
 */
RrefResult rref(const std::vector<std::vector<Rational>> &input) {
  if (input.empty()) {
    return {};
  }

  std::vector<std::vector<Rational>> m = input; // Working copy
  const size_t rows = m.size();
  const size_t cols = m[0].size();
  std::vector<size_t> pivot_cols; // Track which columns have pivots

  size_t r = 0; // Current row (rank)
  for (size_t c = 0; c < cols && r < rows; ++c) {
    // Find pivot: first non-zero entry in column c from row r onward
    size_t pivot = r;
    while (pivot < rows && m[pivot][c] == Rational(0)) {
      ++pivot;
    }
    if (pivot == rows) {
      // No pivot in this column, move to next column
      continue;
    }
    // Swap pivot row to current position
    std::swap(m[r], m[pivot]);

    // Normalize pivot row: divide by pivot value to make pivot = 1
    Rational pivot_val = m[r][c];
    for (size_t k = c; k < cols; ++k) {
      m[r][k] /= pivot_val;
    }

    // Eliminate entries in column c for all other rows
    for (size_t i = 0; i < rows; ++i) {
      if (i == r)
        continue; // Skip pivot row
      Rational factor = m[i][c];
      if (factor == Rational(0))
        continue; // Already zero
      // Subtract factor × pivot_row to eliminate entry in column c
      for (size_t k = c; k < cols; ++k) {
        m[i][k] -= factor * m[r][k];
      }
    }

    pivot_cols.push_back(c); // Record pivot column
    ++r;                     // Move to next row
  }

  return {m, pivot_cols};
}

} // namespace SymAbs
