#pragma once

/**
 * @file Polyhedron.h
 * @brief Convex polyhedral abstraction (Algorithms 9 & 10: α_conv^V,
 * relax-conv)
 */

#include "Solvers/SMT/SymAbs/Affine.h"
#include "Solvers/SMT/SymAbs/Config.h"
#include "Solvers/SMT/SymAbs/LinearExpression.h"

#include <vector>

#include <z3++.h>

namespace SymAbs {

/**
 * @brief Compute convex polyhedral abstraction α_conv^V(φ)
 *
 * Algorithm 9: Computes a convex polyhedron over-approximating φ by iteratively
 * finding extremal points.
 *
 * @param phi The formula
 * @param variables The set of variables V
 * @param config Configuration
 * @return Vector of linear inequalities representing the polyhedron
 */
std::vector<AffineEquality>
alpha_conv_V(z3::expr phi, const std::vector<z3::expr> &variables,
             const AbstractionConfig &config = AbstractionConfig{});

/**
 * @brief Relax a convex polyhedron using template constraints
 *
 * Algorithm 10: relax-conv(φ, c) → t
 *
 * @param phi The formula
 * @param template_constraints Initial template constraints from c
 * @param config Configuration
 * @return Relaxed polyhedron
 */
std::vector<AffineEquality>
relax_conv(z3::expr phi,
           const std::vector<AffineEquality> &template_constraints,
           const AbstractionConfig &config = AbstractionConfig{});

} // namespace SymAbs
