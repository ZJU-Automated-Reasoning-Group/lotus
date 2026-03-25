/**
 * @file HitAndRun.h
 * @brief Header for Hit-and-Run sampling strategy
 */

#pragma once

#include "Solvers/SMT/SMTSampler/PolySampler/RegionSamplingTypes.h"

#include <cstdint>
#include <random>
#include <vector>

namespace RegionSampling {

/**
 * @brief Performs a single step of the Hit-and-Run random walk algorithm.
 *
 * Hit-and-Run is a Markov Chain Monte Carlo (MCMC) method for generating points
 * uniformly distributed within a convex body (polytope).
 *
 * The algorithm proceeds as follows:
 * 1. Generate a random direction vector uniformly from the unit sphere.
 * 2. Compute the chord (line segment) formed by the intersection of the line
 * passing through the current point in the chosen direction and the polytope
 * defined by the constraints.
 * 3. Sample a new point uniformly from this chord.
 *
 * @param constraints The set of linear constraints (Ax <= b) defining the
 * polytope.
 * @param point [in,out] The current point in the walk. Must be a feasible point
 * inside the polytope. On success, this is updated to the new sample point.
 * @param rng The random number generator (64-bit Mersenne Twister).
 * @return true if the step was successful (a valid next point was found), false
 * otherwise.
 */
bool hit_and_run_step(const std::vector<LinearConstraint> &constraints,
                      std::vector<int64_t> &point, std::mt19937_64 &rng);

} // namespace RegionSampling
