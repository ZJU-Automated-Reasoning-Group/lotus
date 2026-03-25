/**
 * @file PolySampler.cpp
 * @brief Sampling loop for polytopes with pluggable walk strategies
 *
 * Fixes applied:
 *  B29 – Burn-in now checks the return value of walk_step and counts only
 *        successful steps, so the chain actually mixes during burn-in even
 *        when individual steps fail.
 *  B30 – The max_attempts_factor termination condition now counts only
 *        *rejected* (duplicate or non-accepted) attempts, not every iteration,
 *        so successful samples do not consume the attempt budget.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/PolySampler.h"

#include "Solvers/SMT/SMTSampler/PolySampler/BallWalk.h"
#include "Solvers/SMT/SMTSampler/PolySampler/ConstraintWalk.h"
#include "Solvers/SMT/SMTSampler/PolySampler/CoordinateWalk.h"
#include "Solvers/SMT/SMTSampler/PolySampler/DikinWalk.h"
#include "Solvers/SMT/SMTSampler/PolySampler/HitAndRun.h"

#include <chrono>
#include <sstream>
#include <unordered_set>

namespace RegionSampling {
namespace {

static std::string point_key(const std::vector<int64_t> &point) {
  std::ostringstream oss;
  for (size_t i = 0; i < point.size(); ++i) {
    if (i)
      oss << ",";
    oss << point[i];
  }
  return oss.str();
}

/**
 * @brief Executes a single step of the random walk.
 *
 * Dispatches the call to the appropriate walk implementation based on the
 * `walk` type.
 *
 * @param constraints The set of linear constraints defining the polytope.
 * @param point [in,out] The current point, updated to the next point.
 * @param walk The type of random walk to perform.
 * @param rng The random number generator.
 * @return true if the step was successful (point updated), false otherwise.
 */
static bool walk_step(const std::vector<LinearConstraint> &constraints,
                      std::vector<int64_t> &point, Walk walk,
                      std::mt19937_64 &rng) {
  switch (walk) {
  case Walk::HitAndRun:
    return RegionSampling::hit_and_run_step(constraints, point, rng);
  case Walk::Dikin:
    return RegionSampling::dikin_walk_step(constraints, point, rng);
  case Walk::Coordinate:
    return RegionSampling::coordinate_walk_step(constraints, point, rng);
  case Walk::Constraint:
    return RegionSampling::constraint_walk_step(constraints, point, rng);
  case Walk::Ball:
    return RegionSampling::ball_walk_step(constraints, point, rng);
  }
  return RegionSampling::coordinate_walk_step(constraints, point, rng);
}

} // namespace

/**
 * @brief Samples points from a polytope defined by linear constraints.
 *
 * 1. Performs a "burn-in" phase to mix the Markov chain.
 * 2. Iteratively generates samples using the specified random walk.
 * 3. Skips a number of steps (`steps_per_sample`) between samples to reduce
 *    correlation.
 * 4. Filters samples using the `accept` predicate.
 * 5. Avoids duplicate samples.
 *
 * @param constraints The linear constraints defining the polytope.
 * @param point The starting point (must be inside the polytope).
 * @param walk The random walk strategy to use.
 * @param rng Random number generator.
 * @param config Configuration parameters.
 * @param accept Predicate to accept or reject a generated sample.
 * @return A vector of sampled points.
 */
std::vector<std::vector<int64_t>>
sample_points(const std::vector<LinearConstraint> &constraints,
              std::vector<int64_t> point, Walk walk, std::mt19937_64 &rng,
              const SampleConfig &config,
              const std::function<bool(const std::vector<int64_t> &)> &accept) {
  std::vector<std::vector<int64_t>> samples;
  if (constraints.empty() || point.empty())
    return samples;

  // Fix B29: burn-in counts only *successful* steps so the chain actually
  // mixes.  We allow up to 10× the requested burn-in steps to account for
  // failed walk attempts.
  {
    int successful = 0;
    const int max_burn_attempts = config.burn_in_steps * 10;
    for (int i = 0; i < max_burn_attempts && successful < config.burn_in_steps;
         ++i) {
      if (walk_step(constraints, point, walk, rng))
        ++successful;
    }
  }

  std::unordered_set<std::string> seen;
  auto start = std::chrono::high_resolution_clock::now();

  // Fix B30: track rejected attempts separately from total iterations so that
  // successful samples do not consume the rejection budget.
  int rejected_attempts = 0;
  const int max_rejected = config.max_samples * config.max_attempts_factor;

  while (static_cast<int>(samples.size()) < config.max_samples) {
    auto now = std::chrono::high_resolution_clock::now();
    double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - start).count();
    if (elapsed_ms > config.max_time_ms)
      break;

    if (rejected_attempts > max_rejected)
      break;

    // Thinning: take multiple steps to reduce correlation between samples.
    // Fix PS-1: count only *successful* steps so that a failed intermediate
    // step does not silently shorten the thinning chain and increase
    // correlation between consecutive samples.
    {
      int thinned = 0;
      int thin_attempts = 0;
      const int max_thin_attempts = config.steps_per_sample * 10;
      while (thinned < config.steps_per_sample &&
             thin_attempts < max_thin_attempts) {
        ++thin_attempts;
        if (walk_step(constraints, point, walk, rng))
          ++thinned;
      }
    }

    std::string key = point_key(point);
    if (seen.find(key) != seen.end()) {
      ++rejected_attempts;
      continue;
    }
    seen.insert(key);

    if (!accept || accept(point)) {
      samples.push_back(point);
    } else {
      ++rejected_attempts;
    }
  }

  return samples;
}

} // namespace RegionSampling
