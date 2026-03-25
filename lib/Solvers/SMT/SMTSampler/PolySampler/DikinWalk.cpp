/**
 * @file DikinWalk.cpp
 * @brief Dikin-style walk with diagonal metric approximation
 *
 * Fixes applied:
 *  B24 – Direction scaled by 1/sqrt(diag) then rounded; replaced with a
 *        Gaussian draw scaled by 1/sqrt(diag) and clamped to [-3,3] before
 *        rounding, reducing the probability of all-zero directions.
 *  B25/B26 – t_low / t_high cast via WalkUtils::safe_cast_t (UB fix).
 *  B27 – slack == 0 (boundary point) no longer causes immediate return;
 *        instead a small epsilon is used so the walk can step away from the
 *        boundary.
 *  L10 – dot_ld moved to WalkUtils.h; local duplicate removed.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/DikinWalk.h"

#include "Solvers/SMT/SMTSampler/PolySampler/WalkUtils.h"

#include <cmath>
#include <limits>

namespace RegionSampling {

bool dikin_walk_step(const std::vector<LinearConstraint> &constraints,
                     std::vector<int64_t> &point, std::mt19937_64 &rng) {
  if (point.empty())
    return false;

  const size_t n = point.size();

  // Compute diagonal of the log-barrier Hessian: sum_i (a_ij^2 / slack_i^2).
  // Fix B27: use a small epsilon instead of returning false when slack == 0,
  // so that boundary points can still move.
  // Fix PS-3: if slack is genuinely negative (the integer point is outside the
  // polytope due to floating-point rounding in dot_ld), return false rather
  // than silently clamping to kEps and producing an invalid sample.
  constexpr long double kEps = 1.0e-6L;
  std::vector<long double> diag(n, 0.0L);
  for (const auto &c : constraints) {
    long double slack =
        static_cast<long double>(c.bound) - WalkUtils::dot_ld(c.coeffs, point);
    if (slack < -kEps)
      return false; // Fix PS-3: genuinely outside polytope — abort step.
    if (slack < kEps)
      slack = kEps; // B27: on boundary — use epsilon to avoid division by zero.
    long double inv = 1.0L / (slack * slack);
    for (size_t j = 0; j < n; ++j) {
      long double a = static_cast<long double>(c.coeffs[j]);
      diag[j] += a * a * inv;
    }
  }
  for (size_t j = 0; j < n; ++j) {
    if (diag[j] <= 0.0L)
      diag[j] = 1.0L;
  }

  // Generate direction: N(0, 1/diag[j]) per coordinate, then round.
  // Fix B24: clamp the raw Gaussian to [-3, 3] before rounding to reduce the
  // probability of rounding to zero (which was ~39% per coordinate with
  // N(0,1)).
  std::normal_distribution<double> normal(0.0, 1.0);
  std::vector<int64_t> direction(n, 0);
  bool non_zero = false;
  for (int attempt = 0; attempt < 32 && !non_zero; ++attempt) {
    non_zero = false;
    for (size_t j = 0; j < n; ++j) {
      long double scale = 1.0L / std::sqrt(diag[j]);
      double raw = normal(rng);
      // Clamp to [-3, 3] so that rounding to zero is less likely.
      if (raw > 3.0)
        raw = 3.0;
      if (raw < -3.0)
        raw = -3.0;
      // Ensure at least ±1 when the raw value is in (-1,1) \ {0} by biasing
      // toward the nearest non-zero integer.
      long double scaled = static_cast<long double>(raw) * scale;
      int64_t v;
      if (scaled > 0.0L && scaled < 1.0L)
        v = 1;
      else if (scaled < 0.0L && scaled > -1.0L)
        v = -1;
      else
        v = static_cast<int64_t>(std::llround(scaled));
      direction[j] = v;
      if (v != 0)
        non_zero = true;
    }
  }
  if (!non_zero)
    return false;

  // Find chord [t_min, t_max].
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
      return false;
    }
  }

  if (!std::isfinite(t_min) || !std::isfinite(t_max) || t_min > t_max)
    return false;

  long double t_low_ld = std::ceil(t_min);
  long double t_high_ld = std::floor(t_max);
  if (t_low_ld > t_high_ld)
    return false;

  // Fix B25/B26: safe cast.
  int64_t t_low = 0, t_high = 0;
  if (!WalkUtils::safe_cast_t(t_low_ld, t_low) ||
      !WalkUtils::safe_cast_t(t_high_ld, t_high))
    return false;

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
