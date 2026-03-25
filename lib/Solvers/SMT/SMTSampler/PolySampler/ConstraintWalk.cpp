/**
 * @file ConstraintWalk.cpp
 * @brief Walk that moves along constraint normals
 *
 * Fixes applied:
 *  B25/B26 – t_low / t_high cast via WalkUtils::safe_cast_t (UB fix).
 *  L10 – dot_ld moved to WalkUtils.h; local duplicate removed.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/ConstraintWalk.h"

#include "Solvers/SMT/SMTSampler/PolySampler/WalkUtils.h"

#include <cmath>
#include <limits>

namespace RegionSampling {

bool constraint_walk_step(const std::vector<LinearConstraint> &constraints,
                          std::vector<int64_t> &point, std::mt19937_64 &rng) {
  if (point.empty() || constraints.empty())
    return false;

  const size_t n = point.size();
  std::uniform_int_distribution<size_t> constraint_dist(0,
                                                        constraints.size() - 1);
  std::uniform_int_distribution<int> sign_dist(0, 1);

  for (int attempt = 0; attempt < 8; ++attempt) {
    const auto &picked = constraints[constraint_dist(rng)];
    std::vector<int64_t> direction = picked.coeffs;
    if (direction.size() != n)
      continue;
    int sign = sign_dist(rng) ? 1 : -1;
    bool non_zero = false;
    for (size_t i = 0; i < n; ++i) {
      direction[i] *= sign;
      if (direction[i] != 0)
        non_zero = true;
    }
    if (!non_zero)
      continue;

    long double t_min = -std::numeric_limits<long double>::infinity();
    long double t_max = std::numeric_limits<long double>::infinity();
    bool feasible = true;

    for (const auto &c : constraints) {
      long double a_dot_x = WalkUtils::dot_ld(c.coeffs, point);
      long double a_dot_d = WalkUtils::dot_ld(c.coeffs, direction);
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
    for (int tries = 0; tries < 4; ++tries) {
      t = dist(rng);
      if (t != 0)
        break;
    }
    if (t == 0)
      continue;

    std::vector<int64_t> candidate(point);
    bool overflow = false;
    for (size_t i = 0; i < n; ++i) {
      __int128 next = static_cast<__int128>(point[i]) +
                      static_cast<__int128>(t) * direction[i];
      if (next < std::numeric_limits<int64_t>::min() ||
          next > std::numeric_limits<int64_t>::max()) {
        overflow = true;
        break;
      }
      candidate[i] = static_cast<int64_t>(next);
    }

    if (!overflow) {
      point.swap(candidate);
      return true;
    }
  }

  return false;
}

} // namespace RegionSampling
