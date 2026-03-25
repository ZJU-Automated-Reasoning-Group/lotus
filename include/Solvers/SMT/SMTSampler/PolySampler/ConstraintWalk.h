/**
 * @file ConstraintWalk.h
 * @brief Header for Constraint Walk sampling strategy
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <random>
#include <vector>

namespace RegionSampling {

/**
 * @brief Performs a single step of the Constraint Walk algorithm.
 *
 * The Constraint Walk is a heuristic random walk where the search direction is
 * chosen based on the normal vectors of the polytope's constraints.
 *
 * The step involves:
 * 1. Randomly selecting one of the linear constraints defining the polytope.
 * 2. Using the normal vector of the selected constraint (its coefficients) as
 * the search direction.
 * 3. Computing the line segment (chord) along this direction within the
 * polytope.
 * 4. Sampling a new point uniformly from this segment.
 *
 * This strategy can be useful for exploring "corners" or boundaries of the
 * polytope defined by specific constraints.
 *
 * @param constraints The set of linear constraints (Ax <= b) defining the
 * polytope.
 * @param point [in,out] The current point in the walk. Must be a feasible point
 * inside the polytope. On success, this is updated to the new sample point.
 * @param rng The random number generator (64-bit Mersenne Twister).
 * @return true if the step was successful, false otherwise.
 */
bool constraint_walk_step(const std::vector<LinearConstraint> &constraints,
                          std::vector<int64_t> &point, std::mt19937_64 &rng);

} // namespace RegionSampling
