/**
 * @file PolySampler.h
 * @brief Main interface for polytope sampling
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <functional>
#include <random>
#include <vector>

namespace RegionSampling {

/**
 * @brief Available random walk strategies for sampling.
 */
enum class Walk {
  HitAndRun, ///< Hit-and-Run algorithm (good for general convex bodies)
  Dikin,     ///< Dikin Walk (adapts to local geometry, good for ill-conditioned
             ///< shapes)
  Coordinate, ///< Coordinate Hit-and-Run (modifies one coordinate at a time)
  Constraint, ///< Constraint Walk (not fully implemented/experimental)
  Ball        ///< Ball Walk (simple local random walk)
};

/**
 * @brief Configuration parameters for the sampling process.
 */
struct SampleConfig {
  int max_samples = 1000;       ///< Maximum number of samples to generate
  double max_time_ms = 30000.0; ///< Maximum execution time in milliseconds
  int burn_in_steps = 20; ///< Number of initial steps to discard (mixing time)
  int steps_per_sample =
      5; ///< Number of walk steps between collected samples (thinning)
  int max_attempts_factor =
      100; ///< Max attempts multiplier relative to max_samples
};

/**
 * @brief Generates samples from a polytope defined by linear constraints.
 *
 * This function performs a random walk starting from an initial feasible point
 * to generate a sequence of points approximately uniformly distributed within
 * the polytope.
 *
 * @param constraints A vector of linear constraints (Ax <= b) defining the
 * polytope.
 * @param point The initial feasible point to start the walk. Must satisfy all
 * constraints.
 * @param walk The random walk strategy to use (e.g., HitAndRun, Dikin).
 * @param rng A 64-bit Mersenne Twister random number generator.
 * @param config Configuration for sampling (count, timeouts, burn-in, etc.).
 * @param accept A predicate function `bool(const std::vector<int64_t>&)` to
 * filter samples. Only points for which this returns `true` are added to the
 * result. Pass `nullptr` or a function always returning `true` to accept all
 * valid points.
 * @return A vector of sampled points (each point is a vector of int64_t
 * coordinates).
 */
std::vector<std::vector<int64_t>>
sample_points(const std::vector<LinearConstraint> &constraints,
              std::vector<int64_t> point, Walk walk, std::mt19937_64 &rng,
              const SampleConfig &config,
              const std::function<bool(const std::vector<int64_t> &)> &accept);

} // namespace RegionSampling
