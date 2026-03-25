/**
 * @file HitAndRun.cpp
 * @brief Integer hit-and-run walk for linear constraints
 *
 * Fixes applied:
 *  B24 – Direction now sampled from {-1, 0, +1} per coordinate using a
 *        uniform distribution instead of rounding a Gaussian, which was
 *        heavily biased toward zero and caused frequent all-zero directions.
 *  B25 – t_low / t_high are now cast via WalkUtils::safe_cast_t which checks
 *        for out-of-range long double values before converting to int64_t,
 *        eliminating the undefined-behaviour cast.
 *  L10 – dot_ld and constraint helpers moved to WalkUtils.h; local duplicate
 *        removed.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/HitAndRun.h"

#include "Solvers/SMT/SMTSampler/PolySampler/WalkUtils.h"

#include <cmath>
#include <limits>

namespace RegionSampling {

bool hit_and_run_step(const std::vector<LinearConstraint> &constraints,
                      std::vector<int64_t> &point, std::mt19937_64 &rng) {
  if (point.empty())
    return false;

  const size_t n = point.size();

  // 1. Generate a random direction vector.
  //
  //    Fix B24: sample each coordinate uniformly from {-1, 0, +1} rather than
  //    rounding a N(0,1) variate.  Rounding a standard normal gives P(0) ≈ 39%
  //    per coordinate, making all-zero directions very likely in high
  //    dimensions. A uniform draw from {-1,0,+1} gives P(0) = 1/3, which is
  //    much better, and the direction is still unbiased (symmetric around
  //    zero).
  // Fix PS-2: non_zero must NOT be reset inside the loop body — doing so
  // makes the outer condition (!non_zero) always true at loop entry, causing
  // all 32 attempts to run even when the first attempt succeeded.
  // The reset is now removed; non_zero is only set to true when a non-zero
  // coordinate is found, and the loop exits as soon as that happens.
  std::uniform_int_distribution<int> dir_dist(-1, 1);
  std::vector<int64_t> direction(n, 0);
  bool non_zero = false;
  for (int attempt = 0; attempt < 32 && !non_zero; ++attempt) {
    // Do NOT reset non_zero here — that was the bug (PS-2).
    for (size_t i = 0; i < n; ++i) {
      int64_t v = static_cast<int64_t>(dir_dist(rng));
      direction[i] = v;
      if (v != 0)
        non_zero = true;
    }
  }
  if (!non_zero)
    return false;

  // 2. Find the chord [t_min, t_max] along the direction.
  long double t_min = -std::numeric_limits<long double>::infinity();
  long double t_max = std::numeric_limits<long double>::infinity();

  for (const auto &c : constraints) {
    long double a_dot_x = WalkUtils::dot_ld(c.coeffs, point);
    long double a_dot_d = WalkUtils::dot_ld(c.coeffs, direction);
    long double slack = static_cast<long double>(c.bound) - a_dot_x;

    if (a_dot_d > 0.0L) {
      t_max = std::min(t_max, slack / a_dot_d);
    } else if (a_dot_d < 0.0L) {
      t_min = std::max(t_min, slack / a_dot_d);
    } else if (slack < 0.0L) {
      // Constraint violated regardless of t – current point is outside
      // polytope.
      return false;
    }
  }

  if (!std::isfinite(t_min) || !std::isfinite(t_max) || t_min > t_max)
    return false;

  // Fix B25: use safe_cast_t to avoid UB when t_low_ld / t_high_ld are outside
  // the representable range of int64_t.
  long double t_low_ld = std::ceil(t_min);
  long double t_high_ld = std::floor(t_max);
  if (t_low_ld > t_high_ld)
    return false;

  int64_t t_low = 0, t_high = 0;
  if (!WalkUtils::safe_cast_t(t_low_ld, t_low) ||
      !WalkUtils::safe_cast_t(t_high_ld, t_high))
    return false;

  // 3. Sample t uniformly from [t_low, t_high].
  std::uniform_int_distribution<int64_t> dist(t_low, t_high);
  int64_t t = dist(rng);

  for (size_t i = 0; i < n; ++i) {
    __int128 next = static_cast<__int128>(point[i]) +
                    static_cast<__int128>(t) * direction[i];
    if (next < std::numeric_limits<int64_t>::min() ||
        next > std::numeric_limits<int64_t>::max())
      return false;
    point[i] = static_cast<int64_t>(next);
  }

  return true;
}

} // namespace RegionSampling
