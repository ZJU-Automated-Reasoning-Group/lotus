/**
 * @file CoordinateWalk.cpp
 * @brief Axis-aligned walk for linear constraints
 *
 * Fixes applied:
 *  B25/B26 – t_low / t_high cast via WalkUtils::safe_cast_t (UB fix).
 *  B31 – The walk now retries with a freshly chosen axis (up to n attempts)
 *        instead of only trying the two sign directions of a single axis.
 *  L10 – dot_ld moved to WalkUtils.h; local duplicate removed.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/CoordinateWalk.h"

#include "Solvers/SMT/SMTSampler/PolySampler/WalkUtils.h"

#include <cmath>
#include <limits>

namespace RegionSampling {

bool coordinate_walk_step(const std::vector<LinearConstraint> &constraints,
                          std::vector<int64_t> &point, std::mt19937_64 &rng) {
  if (point.empty())
    return false;

  const size_t n = point.size();
  std::uniform_int_distribution<size_t> axis_dist(0, n - 1);

  // Fix B31: try up to min(n, 16) different axes instead of only 2 sign
  // attempts on a single axis.
  const int max_axis_attempts = static_cast<int>(std::min(n, size_t(16)));

  for (int axis_attempt = 0; axis_attempt < max_axis_attempts; ++axis_attempt) {
    size_t axis = axis_dist(rng);

    // Try both sign directions for this axis.
    for (int sign_attempt = 0; sign_attempt < 2; ++sign_attempt) {
      int sign = (sign_attempt == 0) ? 1 : -1;

      long double t_min = -std::numeric_limits<long double>::infinity();
      long double t_max = std::numeric_limits<long double>::infinity();
      bool feasible = true;

      for (const auto &c : constraints) {
        long double a_dot_x = WalkUtils::dot_ld(c.coeffs, point);
        long double a_dot_d = static_cast<long double>(c.coeffs[axis]) * sign;
        long double slack = static_cast<long double>(c.bound) - a_dot_x;

        if (a_dot_d > 0.0L) {
          t_max = std::min(t_max, slack / a_dot_d);
        } else if (a_dot_d < 0.0L) {
          t_min = std::max(t_min, slack / a_dot_d);
        } else if (slack < 0.0L) {
          feasible = false;
          break;
        }
      }

      if (!feasible || !std::isfinite(t_min) || !std::isfinite(t_max) ||
          t_min > t_max)
        continue;

      long double t_low_ld = std::ceil(t_min);
      long double t_high_ld = std::floor(t_max);
      if (t_low_ld > t_high_ld)
        continue;

      // Fix B25/B26: safe cast.
      int64_t t_low = 0, t_high = 0;
      if (!WalkUtils::safe_cast_t(t_low_ld, t_low) ||
          !WalkUtils::safe_cast_t(t_high_ld, t_high))
        continue;

      if (t_low == 0 && t_high == 0)
        continue;

      std::uniform_int_distribution<int64_t> dist(t_low, t_high);
      int64_t t = 0;
      for (int tries = 0; tries < 8; ++tries) {
        t = dist(rng);
        if (t != 0)
          break;
      }
      if (t == 0)
        continue;

      __int128 next =
          static_cast<__int128>(point[axis]) + static_cast<__int128>(t) * sign;
      if (next < std::numeric_limits<int64_t>::min() ||
          next > std::numeric_limits<int64_t>::max())
        continue;

      point[axis] = static_cast<int64_t>(next);
      return true;
    }
  }

  return false;
}

} // namespace RegionSampling
