/**
 * @file BallWalk.cpp
 * @brief Local lattice ball walk for linear constraints
 *
 * Fixes applied:
 *  B28 – Step size is now adaptive: it is computed as max(1, range/1000) where
 *        range is the span of the polytope along each axis, so the walk scales
 *        with the problem rather than using a hardcoded ±2.
 *  L10 – dot_ld / satisfies_constraints moved to WalkUtils.h; local duplicates
 *        removed.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/BallWalk.h"

#include "Solvers/SMT/SMTSampler/PolySampler/WalkUtils.h"

#include <cmath>
#include <limits>

namespace RegionSampling {

bool ball_walk_step(const std::vector<LinearConstraint> &constraints,
                    std::vector<int64_t> &point, std::mt19937_64 &rng) {
  if (point.empty() || constraints.empty())
    return false;

  const size_t n = point.size();

  // Fix B28: compute an adaptive step size per dimension.
  // Estimate the range of each coordinate from the constraint bounds.
  // For each axis j, find the tightest upper and lower bounds implied by
  // constraints whose coefficient vector is a unit vector along j.
  // Fall back to a default of 1 if no axis-aligned constraints exist.
  std::vector<int64_t> step_size(n, 1);
  {
    std::vector<long double> lo(n, -1e18L), hi(n, 1e18L);
    for (const auto &c : constraints) {
      // Check if this is an axis-aligned constraint (only one non-zero coeff).
      int nonzero_count = 0;
      size_t nonzero_idx = 0;
      int64_t nonzero_coeff = 0;
      for (size_t j = 0; j < n; ++j) {
        if (c.coeffs[j] != 0) {
          ++nonzero_count;
          nonzero_idx = j;
          nonzero_coeff = c.coeffs[j];
        }
      }
      if (nonzero_count == 1) {
        long double bound = static_cast<long double>(c.bound) /
                            static_cast<long double>(nonzero_coeff);
        if (nonzero_coeff > 0)
          hi[nonzero_idx] = std::min(hi[nonzero_idx], bound);
        else
          lo[nonzero_idx] = std::max(lo[nonzero_idx], bound);
      }
    }
    for (size_t j = 0; j < n; ++j) {
      if (std::isfinite(lo[j]) && std::isfinite(hi[j]) && hi[j] > lo[j]) {
        long double range = hi[j] - lo[j];
        // Use ~0.1% of the range as the step size, minimum 1.
        // Fix PS-5: range / 1000 can exceed INT64_MAX for very large ranges
        // (e.g., 64-bit variables).  Clamp to INT64_MAX before casting.
        long double raw_step = std::max(1.0L, range / 1000.0L);
        constexpr long double kInt64Max =
            static_cast<long double>(std::numeric_limits<int64_t>::max());
        if (raw_step > kInt64Max)
          raw_step = kInt64Max;
        int64_t s = static_cast<int64_t>(raw_step);
        step_size[j] = s;
      }
    }
  }

  for (int attempt = 0; attempt < 32; ++attempt) {
    std::vector<int64_t> candidate(point);
    bool non_zero = false;
    bool overflow = false;
    for (size_t i = 0; i < n; ++i) {
      std::uniform_int_distribution<int64_t> step_dist(-step_size[i],
                                                       step_size[i]);
      int64_t delta = step_dist(rng);
      if (delta != 0)
        non_zero = true;
      __int128 next =
          static_cast<__int128>(point[i]) + static_cast<__int128>(delta);
      if (next < std::numeric_limits<int64_t>::min() ||
          next > std::numeric_limits<int64_t>::max()) {
        overflow = true;
        break;
      }
      candidate[i] = static_cast<int64_t>(next);
    }
    if (!non_zero || overflow)
      continue;

    if (WalkUtils::satisfies_constraints(constraints, candidate)) {
      point.swap(candidate);
      return true;
    }
  }

  return false;
}

} // namespace RegionSampling
