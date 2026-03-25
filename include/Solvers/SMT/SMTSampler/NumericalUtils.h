/**
 * @file NumericalUtils.h
 * @brief Safe numerical utilities for SMT sampling with bit-vectors
 *
 * This header provides utilities for handling bit-vector arithmetic safely
 * across different widths (1-64 bits), avoiding overflow, truncation, and
 * undefined behavior issues that commonly arise when mapping SMT bit-vectors
 * to native C++ integer types.
 *
 * Key features:
 * - Support for arbitrary-width bit-vectors (1-128 bits)
 * - Overflow-safe arithmetic operations
 * - Proper signed/unsigned semantics matching SMT-LIB
 * - Efficient representation for common cases (≤64 bits)
 */

#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include <z3++.h>

namespace SMTSampler {

/**
 * @brief Simple optional-like wrapper for C++14 compatibility.
 * Use std::optional when upgrading to C++17.
 */
template <typename T> class Optional {
public:
  Optional() : has_value_(false), value_() {}
  Optional(const T &value) : has_value_(true), value_(value) {}

  bool has_value() const { return has_value_; }
  explicit operator bool() const { return has_value_; }

  const T &value() const {
    if (!has_value_)
      throw std::runtime_error("Optional has no value");
    return value_;
  }

  T &value() {
    if (!has_value_)
      throw std::runtime_error("Optional has no value");
    return value_;
  }

  const T &value_or(const T &default_value) const {
    return has_value_ ? value_ : default_value;
  }

private:
  bool has_value_;
  T value_;
};

template <typename T> Optional<T> make_optional(const T &value) {
  return Optional<T>(value);
}

template <typename T> Optional<T> nullopt() { return Optional<T>(); }

/**
 * @brief Represents a bit-vector value with arbitrary width.
 *
 * For widths ≤ 64, uses native uint64_t storage.
 * For widths > 64, uses Z3's arbitrary-precision representation.
 */
class BVValue {
public:
  /**
   * @brief Constructs a BVValue from a native integer.
   * @param value The integer value (will be masked to width)
   * @param width Bit-width (1-128)
   */
  explicit BVValue(uint64_t value = 0, unsigned width = 64);

  /**
   * @brief Constructs a BVValue from a Z3 bit-vector expression.
   * @param bv_expr Z3 bit-vector constant expression
   */
  explicit BVValue(const z3::expr &bv_expr);

  /**
   * @brief Constructs a BVValue from a signed integer.
   * @param value Signed integer value
   * @param width Bit-width (1-128)
   */
  static BVValue from_signed(int64_t value, unsigned width);

  /**
   * @brief Returns the bit-width of this value.
   */
  unsigned width() const { return width_; }

  /**
   * @brief Converts to uint64_t (only valid for width ≤ 64).
   * @throws std::runtime_error if width > 64
   */
  uint64_t to_uint64() const;

  /**
   * @brief Converts to int64_t using signed interpretation (only valid for
   * width ≤ 64).
   * @throws std::runtime_error if width > 64
   */
  int64_t to_int64() const;

  /**
   * @brief Converts to Z3 bit-vector expression.
   */
  z3::expr to_z3_expr(z3::context &ctx) const;

  /**
   * @brief Returns true if this value fits in a uint64_t.
   */
  bool fits_uint64() const { return width_ <= 64; }

  /**
   * @brief Returns true if this value fits in an int64_t (signed).
   */
  bool fits_int64() const;

  /**
   * @brief String representation (decimal).
   */
  std::string to_string() const;

  /**
   * @brief String representation (hexadecimal).
   */
  std::string to_hex_string() const;

private:
  unsigned width_;
  uint64_t value_small_;              // Used when width_ <= 64
  Optional<std::string> value_large_; // Used when width_ > 64 (decimal string)
};

/**
 * @brief Represents a range [min, max] for bit-vector values.
 *
 * Handles both signed and unsigned interpretations, and supports
 * arbitrary-width bit-vectors.
 */
class BVRange {
public:
  /**
   * @brief Constructs an unsigned range [0, 2^width - 1].
   */
  static BVRange unsigned_full(unsigned width);

  /**
   * @brief Constructs a signed range [-2^(width-1), 2^(width-1) - 1].
   */
  static BVRange signed_full(unsigned width);

  /**
   * @brief Constructs a custom range.
   * @param min Minimum value (inclusive)
   * @param max Maximum value (inclusive)
   * @param width Bit-width
   */
  BVRange(BVValue min, BVValue max);

  /**
   * @brief Returns the minimum value.
   */
  const BVValue &min() const { return min_; }

  /**
   * @brief Returns the maximum value.
   */
  const BVValue &max() const { return max_; }

  /**
   * @brief Returns the bit-width.
   */
  unsigned width() const { return min_.width(); }

  /**
   * @brief Returns true if the range is empty (min > max).
   */
  bool is_empty() const;

  /**
   * @brief Returns true if the range contains exactly one value.
   */
  bool is_singleton() const;

  /**
   * @brief Returns the size of the range (max - min + 1).
   * @return Empty Optional if the range is too large to represent
   */
  Optional<uint64_t> size() const;

  /**
   * @brief Samples a random value uniformly from this range.
   * @param rng Random number generator
   * @return A random value in [min, max]
   */
  template <typename RNG> BVValue sample_uniform(RNG &rng) const;

private:
  BVValue min_;
  BVValue max_;
};

/**
 * @brief Safe arithmetic operations for bit-vector sampling.
 */
namespace SafeArithmetic {

/**
 * @brief Computes (a - b) without overflow, using unsigned arithmetic.
 * @return Empty Optional if the result would overflow uint64_t
 */
Optional<uint64_t> safe_subtract(int64_t a, int64_t b);

/**
 * @brief Computes (a + b) without overflow.
 * @return Empty Optional if the result would overflow int64_t
 */
Optional<int64_t> safe_add(int64_t a, int64_t b);

/**
 * @brief Computes 2^width - 1 safely.
 * @return Empty Optional if width > 64
 */
Optional<uint64_t> safe_bv_max(unsigned width);

/**
 * @brief Converts a Z3 numeral to int64_t safely.
 * @return Empty Optional if the value doesn't fit or is unbounded
 */
Optional<int64_t> z3_to_int64(const z3::expr &e);

/**
 * @brief Converts a Z3 numeral to uint64_t safely.
 * @return Empty Optional if the value doesn't fit or is unbounded
 */
Optional<uint64_t> z3_to_uint64(const z3::expr &e);

} // namespace SafeArithmetic

/**
 * @brief Utilities for extracting bounds from Z3 optimization results.
 */
namespace BoundExtraction {

/**
 * @brief Extracts a lower bound from an optimization result.
 * @param opt Z3 optimize object
 * @param handle Optimization handle
 * @param default_value Fallback value if extraction fails
 * @return The extracted bound or default_value
 */
int64_t extract_lower_bound(z3::optimize &opt,
                            const z3::optimize::handle &handle,
                            int64_t default_value = 0);

/**
 * @brief Extracts an upper bound from an optimization result.
 * @param opt Z3 optimize object
 * @param handle Optimization handle
 * @param var The variable being optimized (for fallback to bit-width)
 * @param default_value Fallback value if extraction fails
 * @return The extracted bound or default_value
 */
int64_t extract_upper_bound(z3::optimize &opt,
                            const z3::optimize::handle &handle,
                            const z3::expr &var,
                            int64_t default_value = INT64_MAX);

/**
 * @brief Extracts bounds for a variable using Z3 optimization.
 * @param formula The SMT formula
 * @param var The variable to bound
 * @param ctx Z3 context
 * @return A BVRange representing the computed bounds
 */
BVRange compute_bounds(const z3::expr &formula, const z3::expr &var,
                       z3::context &ctx);

} // namespace BoundExtraction

/**
 * @brief Utilities for converting between Z3 and native representations.
 */
namespace Conversion {

/**
 * @brief Creates a Z3 bit-vector from an int64_t value.
 * @param ctx Z3 context
 * @param value The integer value
 * @param width Bit-width
 * @return Z3 bit-vector expression
 */
z3::expr int64_to_bv(z3::context &ctx, int64_t value, unsigned width);

/**
 * @brief Creates a Z3 bit-vector from a uint64_t value.
 * @param ctx Z3 context
 * @param value The unsigned integer value
 * @param width Bit-width
 * @return Z3 bit-vector expression
 */
z3::expr uint64_to_bv(z3::context &ctx, uint64_t value, unsigned width);

/**
 * @brief Extracts a value from a Z3 model.
 * @param model Z3 model
 * @param var Variable to extract
 * @param default_value Fallback if extraction fails
 * @return The extracted value or default_value
 */
int64_t extract_from_model(const z3::model &model, const z3::expr &var,
                           int64_t default_value = 0);

} // namespace Conversion

} // namespace SMTSampler
