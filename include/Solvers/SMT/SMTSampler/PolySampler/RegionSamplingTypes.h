/**
 * @file RegionSamplingTypes.h
 * @brief Common type definitions for region sampling
 */

#pragma once

#include <cstdint>
#include <vector>

namespace RegionSampling {

/**
 * @brief Represents a linear constraint of the form: sum(coeffs[i] * x[i]) <=
 * bound
 *
 * This structure is used to define the convex polytope for sampling.
 * The polytope is the intersection of multiple such half-spaces.
 */
struct LinearConstraint {
  std::vector<int64_t>
      coeffs;    ///< Coefficients of the variables (normal vector)
  int64_t bound; ///< The upper bound (RHS) of the inequality
};

} // namespace RegionSampling
