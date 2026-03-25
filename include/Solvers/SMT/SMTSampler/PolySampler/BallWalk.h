/**
 * @file BallWalk.h
 * @brief Header for Ball Walk sampling strategy
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <random>
#include <vector>

namespace RegionSampling {

/**
 * @brief Performs a single step of the Ball Walk algorithm.
 *
 * The Ball Walk is a simple MCMC algorithm where the next point is chosen
 * uniformly from a ball of fixed radius centered at the current point.
 *
 * The algorithm proceeds as follows:
 * 1. Generate a candidate point uniformly from a small box (L-infinity ball) or
 * ball around the current point. In this implementation, it perturbs each
 * coordinate by a small random integer offset.
 * 2. Check if the candidate point satisfies all linear constraints (is inside
 * the polytope).
 * 3. If valid, move to the candidate point (accept); otherwise, stay at the
 * current point (reject). Note: The implementation here returns `false` on
 * rejection/failure to find a valid point after retries, effectively staying at
 * the same point from the caller's perspective (or retrying).
 *
 * @param constraints The set of linear constraints (Ax <= b) defining the
 * polytope.
 * @param point [in,out] The current point in the walk. Must be a feasible point
 * inside the polytope. On success, this is updated to the new sample point.
 * @param rng The random number generator (64-bit Mersenne Twister).
 * @return true if the step resulted in a move to a new valid point, false
 * otherwise.
 */
bool ball_walk_step(const std::vector<LinearConstraint> &constraints,
                    std::vector<int64_t> &point, std::mt19937_64 &rng);

} // namespace RegionSampling
