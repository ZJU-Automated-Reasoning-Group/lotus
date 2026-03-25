/**
 * @file DikinWalk.h
 * @brief Header for Dikin Walk sampling strategy
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <random>
#include <vector>

namespace RegionSampling {

/**
 * @brief Performs a single step of the Dikin Walk algorithm.
 *
 * The Dikin Walk is a random walk based on the Dikin ellipsoid, which adapts to
 * the local geometry of the polytope. It is generally more efficient than the
 * Ball Walk or Coordinate Walk for ill-conditioned polytopes (long and thin
 * shapes).
 *
 * This implementation uses a diagonal approximation of the Hessian of the
 * logarithmic barrier function to define the local metric (ellipsoid).
 *
 * The step involves:
 * 1. Computing the diagonal elements of the Hessian at the current point.
 * 2. Generating a random direction scaled by the inverse square root of these
 * diagonal elements (effectively sampling from the local ellipsoid).
 * 3. Finding the intersection of the line along this direction with the
 * polytope boundaries.
 * 4. Sampling a new point along the valid segment.
 *
 * @param constraints The set of linear constraints (Ax <= b) defining the
 * polytope.
 * @param point [in,out] The current point in the walk. Must be a feasible point
 * inside the polytope. On success, this is updated to the new sample point.
 * @param rng The random number generator (64-bit Mersenne Twister).
 * @return true if the step was successful, false otherwise.
 */
bool dikin_walk_step(const std::vector<LinearConstraint> &constraints,
                     std::vector<int64_t> &point, std::mt19937_64 &rng);

} // namespace RegionSampling
