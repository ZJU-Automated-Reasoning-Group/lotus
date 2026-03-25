/**
 * @file CoordinateWalk.h
 * @brief Header for Coordinate Hit-and-Run sampling strategy
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <random>
#include <vector>

namespace RegionSampling {

/**
 * @brief Performs a single step of the Coordinate Hit-and-Run algorithm.
 *
 * This variant of the Hit-and-Run algorithm restricts the search direction to
 * be aligned with one of the coordinate axes.
 *
 * The step involves:
 * 1. Randomly selecting one of the coordinate axes (dimensions).
 * 2. Defining a line passing through the current point parallel to the selected
 * axis.
 * 3. Computing the intersection of this line with the polytope boundaries.
 * 4. Sampling a new point uniformly from the valid segment along this axis.
 *
 * This method is computationally cheaper per step than general Hit-and-Run
 * since it only modifies one coordinate at a time, but it may mix more slowly
 * if the polytope is not axis-aligned.
 *
 * @param constraints The set of linear constraints (Ax <= b) defining the
 * polytope.
 * @param point [in,out] The current point in the walk. Must be a feasible point
 * inside the polytope. On success, this is updated to the new sample point.
 * @param rng The random number generator (64-bit Mersenne Twister).
 * @return true if the step was successful (a valid next point was found), false
 * otherwise.
 */
bool coordinate_walk_step(const std::vector<LinearConstraint> &constraints,
                          std::vector<int64_t> &point, std::mt19937_64 &rng);

} // namespace RegionSampling
