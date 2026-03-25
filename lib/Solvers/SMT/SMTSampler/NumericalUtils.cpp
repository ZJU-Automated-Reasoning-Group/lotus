/**
 * @file NumericalUtils.cpp
 * @brief Implementation of safe numerical utilities for SMT sampling
 */

#include "Solvers/SMT/SMTSampler/NumericalUtils.h"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <sstream>

namespace SMTSampler {

// ============================================================================
// BVValue Implementation
// ============================================================================

BVValue::BVValue(uint64_t value, unsigned width)
    : width_(width), value_small_(0) {
  if (width == 0 || width > 128) {
    throw std::invalid_argument("BVValue width must be in [1, 128]");
  }

  if (width <= 64) {
    // Mask to width
    if (width < 64) {
      uint64_t mask = (1ULL << width) - 1ULL;
      value_small_ = value & mask;
    } else {
      value_small_ = value;
    }
  } else {
    // For width > 64, store as string (simplified implementation)
    // In production, you'd use GMP or Z3's internal representation
    value_large_ = std::to_string(value);
  }
}

BVValue::BVValue(const z3::expr &bv_expr) {
  if (!bv_expr.is_bv() || !bv_expr.is_numeral()) {
    throw std::invalid_argument("BVValue requires a bit-vector numeral");
  }

  width_ = bv_expr.get_sort().bv_size();

  if (width_ <= 64) {
    value_small_ = bv_expr.get_numeral_uint64();
  } else {
    // For wide bit-vectors, use string representation
    value_large_ = bv_expr.get_decimal_string(10);
  }
}

BVValue BVValue::from_signed(int64_t value, unsigned width) {
  if (width == 0 || width > 128) {
    throw std::invalid_argument("BVValue width must be in [1, 128]");
  }

  // Convert signed to unsigned representation
  uint64_t unsigned_value = static_cast<uint64_t>(value);

  // Mask to width
  if (width < 64) {
    uint64_t mask = (1ULL << width) - 1ULL;
    unsigned_value &= mask;
  }

  return BVValue(unsigned_value, width);
}

uint64_t BVValue::to_uint64() const {
  if (width_ > 64) {
    throw std::runtime_error(
        "BVValue too wide for uint64_t (width=" + std::to_string(width_) + ")");
  }
  return value_small_;
}

int64_t BVValue::to_int64() const {
  if (width_ > 64) {
    throw std::runtime_error(
        "BVValue too wide for int64_t (width=" + std::to_string(width_) + ")");
  }

  // Check if the value fits in int64_t when interpreted as signed
  if (width_ == 64) {
    // For 64-bit values, just reinterpret
    return static_cast<int64_t>(value_small_);
  } else {
    // Check if the sign bit is set
    uint64_t sign_bit = 1ULL << (width_ - 1);
    if (value_small_ & sign_bit) {
      // Negative value: sign-extend
      uint64_t extension = ~((1ULL << width_) - 1ULL);
      return static_cast<int64_t>(value_small_ | extension);
    } else {
      // Positive value
      return static_cast<int64_t>(value_small_);
    }
  }
}

z3::expr BVValue::to_z3_expr(z3::context &ctx) const {
  if (width_ <= 64) {
    return ctx.bv_val(value_small_, width_);
  } else {
    // For wide bit-vectors, use string constructor
    return ctx.bv_val(value_large_.value().c_str(), width_);
  }
}

bool BVValue::fits_int64() const {
  if (width_ > 64) {
    return false;
  }

  if (width_ == 64) {
    // All 64-bit values fit when reinterpreted
    return true;
  }

  // Check if the value is within int64_t range
  uint64_t sign_bit = 1ULL << (width_ - 1);
  if (value_small_ & sign_bit) {
    // Negative value: check if it's >= INT64_MIN
    int64_t signed_val = to_int64();
    return signed_val >= std::numeric_limits<int64_t>::min();
  } else {
    // Positive value: check if it's <= INT64_MAX
    return value_small_ <=
           static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  }
}

std::string BVValue::to_string() const {
  if (width_ <= 64) {
    return std::to_string(value_small_);
  } else {
    return value_large_.value();
  }
}

std::string BVValue::to_hex_string() const {
  if (width_ <= 64) {
    std::ostringstream oss;
    oss << "0x" << std::hex << value_small_;
    return oss.str();
  } else {
    // Simplified: just return decimal for wide values
    return value_large_.value();
  }
}

// ============================================================================
// BVRange Implementation
// ============================================================================

BVRange BVRange::unsigned_full(unsigned width) {
  BVValue min(0, width);
  uint64_t max_val;
  if (width >= 64) {
    max_val = std::numeric_limits<uint64_t>::max();
  } else {
    max_val = (1ULL << width) - 1ULL;
  }
  BVValue max(max_val, width);
  return BVRange(min, max);
}

BVRange BVRange::signed_full(unsigned width) {
  if (width == 0) {
    throw std::invalid_argument("Width must be > 0");
  }

  int64_t min_signed, max_signed;
  if (width >= 64) {
    min_signed = std::numeric_limits<int64_t>::min();
    max_signed = std::numeric_limits<int64_t>::max();
  } else {
    int64_t half = 1LL << (width - 1);
    min_signed = -half;
    max_signed = half - 1;
  }

  BVValue min = BVValue::from_signed(min_signed, width);
  BVValue max = BVValue::from_signed(max_signed, width);
  return BVRange(min, max);
}

BVRange::BVRange(BVValue min, BVValue max) : min_(min), max_(max) {
  if (min_.width() != max_.width()) {
    throw std::invalid_argument("BVRange min and max must have same width");
  }
}

bool BVRange::is_empty() const {
  if (!min_.fits_uint64() || !max_.fits_uint64()) {
    // For wide values, we'd need proper comparison
    return false;
  }
  return min_.to_uint64() > max_.to_uint64();
}

bool BVRange::is_singleton() const {
  if (!min_.fits_uint64() || !max_.fits_uint64()) {
    return false;
  }
  return min_.to_uint64() == max_.to_uint64();
}

Optional<uint64_t> BVRange::size() const {
  if (!min_.fits_uint64() || !max_.fits_uint64()) {
    return nullopt<uint64_t>();
  }

  uint64_t min_val = min_.to_uint64();
  uint64_t max_val = max_.to_uint64();

  if (min_val > max_val) {
    return 0; // Empty range
  }

  // Check for overflow
  if (max_val == std::numeric_limits<uint64_t>::max() && min_val == 0) {
    return nullopt<uint64_t>(); // Full range, can't represent size
  }

  uint64_t size = max_val - min_val + 1;
  // Check if addition overflowed (wrapped to 0)
  if (size == 0 && min_val != max_val + 1) {
    return nullopt<uint64_t>();
  }

  return size;
}

template <typename RNG> BVValue BVRange::sample_uniform(RNG &rng) const {
  if (!min_.fits_uint64() || !max_.fits_uint64()) {
    throw std::runtime_error("Cannot sample from wide BVRange (width > 64)");
  }

  uint64_t min_val = min_.to_uint64();
  uint64_t max_val = max_.to_uint64();

  if (min_val > max_val) {
    throw std::runtime_error("Cannot sample from empty range");
  }

  if (min_val == max_val) {
    return min_;
  }

  // Use the safe subtraction method from IntervalSampler
  uint64_t range = max_val - min_val + 1;

  // Handle full range case
  if (range == 0) {
    // Full uint64_t range
    std::uniform_int_distribution<uint64_t> dist(
        std::numeric_limits<uint64_t>::min(),
        std::numeric_limits<uint64_t>::max());
    return BVValue(dist(rng), width());
  }

  std::uniform_int_distribution<uint64_t> dist(0, range - 1);
  uint64_t offset = dist(rng);
  uint64_t result = min_val + offset;

  return BVValue(result, width());
}

// Explicit template instantiation for common RNG types
template BVValue
BVRange::sample_uniform<std::mt19937_64>(std::mt19937_64 &) const;
template BVValue BVRange::sample_uniform<std::mt19937>(std::mt19937 &) const;

// ============================================================================
// SafeArithmetic Implementation
// ============================================================================

namespace SafeArithmetic {

Optional<uint64_t> safe_subtract(int64_t a, int64_t b) {
  // Cast to unsigned to avoid signed overflow
  uint64_t ua = static_cast<uint64_t>(a);
  uint64_t ub = static_cast<uint64_t>(b);

  // Unsigned subtraction never overflows, but may wrap
  uint64_t result = ua - ub;

  // Check if the result makes sense
  // If a >= b (as signed), result should be <= INT64_MAX
  // If a < b (as signed), result will be large (wrapped)
  if (a >= b) {
    // Normal case: positive difference
    if (result > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return nullopt<uint64_t>(); // Overflow
    }
  } else {
    // Wrapped case: this is expected for negative differences
    // The result is valid as uint64_t but may not fit in int64_t
  }

  return result;
}

Optional<int64_t> safe_add(int64_t a, int64_t b) {
  // Check for overflow
  if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) {
    return nullopt<int64_t>();
  }
  if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) {
    return nullopt<int64_t>();
  }
  return a + b;
}

Optional<uint64_t> safe_bv_max(unsigned width) {
  if (width == 0 || width > 64) {
    return nullopt<uint64_t>();
  }
  if (width == 64) {
    return std::numeric_limits<uint64_t>::max();
  }
  return (1ULL << width) - 1ULL;
}

Optional<int64_t> z3_to_int64(const z3::expr &e) {
  if (!e.is_numeral()) {
    return nullopt<int64_t>();
  }

  try {
    if (e.is_int()) {
      return e.get_numeral_int64();
    } else if (e.is_bv()) {
      // For bit-vectors, interpret as signed
      BVValue bv(e);
      if (!bv.fits_int64()) {
        return nullopt<int64_t>();
      }
      return bv.to_int64();
    }
  } catch (const z3::exception &) {
    return nullopt<int64_t>();
  }

  return nullopt<int64_t>();
}

Optional<uint64_t> z3_to_uint64(const z3::expr &e) {
  if (!e.is_numeral()) {
    return nullopt<uint64_t>();
  }

  try {
    if (e.is_int()) {
      int64_t val = e.get_numeral_int64();
      if (val < 0) {
        return nullopt<uint64_t>();
      }
      return static_cast<uint64_t>(val);
    } else if (e.is_bv()) {
      BVValue bv(e);
      if (!bv.fits_uint64()) {
        return nullopt<uint64_t>();
      }
      return bv.to_uint64();
    }
  } catch (const z3::exception &) {
    return nullopt<uint64_t>();
  }

  return nullopt<uint64_t>();
}

} // namespace SafeArithmetic

// ============================================================================
// BoundExtraction Implementation
// ============================================================================

namespace BoundExtraction {

int64_t extract_lower_bound(z3::optimize &opt,
                            const z3::optimize::handle &handle,
                            int64_t default_value) {
  try {
    z3::expr bound_expr = opt.lower(handle);
    auto result = SafeArithmetic::z3_to_int64(bound_expr);
    if (result.has_value()) {
      return result.value();
    }
  } catch (const z3::exception &e) {
    std::cerr << "[BoundExtraction] Failed to extract lower bound: " << e.msg()
              << "; using default " << default_value << "\n";
  }
  return default_value;
}

int64_t extract_upper_bound(z3::optimize &opt,
                            const z3::optimize::handle &handle,
                            const z3::expr &var, int64_t default_value) {
  try {
    z3::expr bound_expr = opt.upper(handle);
    auto result = SafeArithmetic::z3_to_int64(bound_expr);
    if (result.has_value()) {
      return result.value();
    }
  } catch (const z3::exception &e) {
    std::cerr << "[BoundExtraction] Failed to extract upper bound: " << e.msg();
  }

  // Fallback: use bit-width default
  if (var.is_bv()) {
    unsigned width = var.get_sort().bv_size();
    auto max_val = SafeArithmetic::safe_bv_max(width);
    if (max_val.has_value()) {
      // Check if it fits in int64_t
      uint64_t max_uint = max_val.value();
      if (max_uint <=
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        std::cerr << "; using bit-width default " << max_uint << "\n";
        return static_cast<int64_t>(max_uint);
      }
    }
  }

  std::cerr << "; using fallback default " << default_value << "\n";
  return default_value;
}

BVRange extract_range(z3::optimize &opt_min, z3::optimize &opt_max,
                      const z3::optimize::handle &handle_min,
                      const z3::optimize::handle &handle_max,
                      const z3::expr &var, bool use_signed) {
  unsigned width = var.get_sort().bv_size();

  // Try to extract bounds
  int64_t lower = extract_lower_bound(opt_min, handle_min, 0);
  int64_t upper = extract_upper_bound(
      opt_max, handle_max, var,
      use_signed ? std::numeric_limits<int64_t>::max()
                 : static_cast<int64_t>((1ULL << std::min(width, 63u)) - 1));

  BVValue min_val = BVValue::from_signed(lower, width);
  BVValue max_val = BVValue::from_signed(upper, width);

  return BVRange(min_val, max_val);
}

} // namespace BoundExtraction

} // namespace SMTSampler
