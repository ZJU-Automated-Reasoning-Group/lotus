/**
 * @file WalkUtils.h
 * @brief Shared utility functions for polytope walk implementations
 *
 * Centralises helpers that were previously duplicated across every walk .cpp
 * (dot_ld, satisfies_constraints, safe_cast_t).  Including this header in each
 * walk translation unit removes the duplication (fixes L10).
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace RegionSampling {
namespace WalkUtils {

/// Long-double dot product of two int64_t vectors.
inline long double dot_ld(const std::vector<int64_t> &a,
                          const std::vector<int64_t> &b) {
  long double sum = 0.0L;
  for (size_t i = 0; i < a.size(); ++i)
    sum += static_cast<long double>(a[i]) * static_cast<long double>(b[i]);
  return sum;
}

/// Returns true iff every constraint is satisfied by @p point.
inline bool
satisfies_constraints(const std::vector<LinearConstraint> &constraints,
                      const std::vector<int64_t> &point) {
  for (const auto &c : constraints) {
    if (dot_ld(c.coeffs, point) > static_cast<long double>(c.bound))
      return false;
  }
  return true;
}

/**
 * @brief Safely cast a long double to int64_t.
 *
 * Returns false (and leaves @p out unchanged) if the value is not finite or
 * lies outside [INT64_MIN, INT64_MAX].  This fixes the undefined-behaviour
 * casts identified as B25/B26.
 *
 * Fix PS-4: INT64_MAX = 2^63 - 1 is not exactly representable as long double
 * (80-bit extended has a 64-bit mantissa, so it rounds up to 2^63).  The old
 * comparison `v > kMax` therefore passed for values in [2^63-1, 2^63), which
 * could allow an out-of-range cast.  We now use a safe upper sentinel of
 * 9.2233720368547758080e18L (= 2^63 exactly as long double) and require
 * v < sentinel (strict), which correctly excludes all values >= INT64_MAX+1.
 */
inline bool safe_cast_t(long double v, int64_t &out) {
  if (!std::isfinite(v))
    return false;
  // INT64_MIN = -2^63 is exactly representable as long double.
  constexpr long double kMin =
      static_cast<long double>(std::numeric_limits<int64_t>::min());
  // Fix PS-4: use 2^63 (the next representable value above INT64_MAX) as the
  // exclusive upper sentinel.  Any v >= kMaxSentinel cannot be stored in
  // int64_t without overflow.
  constexpr long double kMaxSentinel = 9.2233720368547758080e18L; // 2^63
  if (v < kMin || v >= kMaxSentinel)
    return false;
  out = static_cast<int64_t>(v);
  return true;
}

} // namespace WalkUtils
} // namespace RegionSampling
