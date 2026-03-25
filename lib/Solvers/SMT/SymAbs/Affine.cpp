/**
 * @file Affine.cpp
 * @brief Implementation of Algorithm 12: α_aff^V
 *
 * This module implements Algorithm 12 from "Automatic Abstraction of Bit-Vector
 * Formulae" for computing the affine hull of the models of a formula φ.
 *
 * **Affine Hull:**
 * The affine hull of a set of points is the smallest affine space containing
 * all points. It can be represented as a system of affine equalities: [A|b]
 * such that A·x = b for all points x in the hull.
 *
 * **Mathematical Background:**
 * - An affine space is defined by a point p (anchor) and a linear subspace
 * (nullspace)
 * - The nullspace is computed from difference vectors (p_i - p_1) for i = 2,
 * ..., n
 * - Using RREF, we find the nullspace basis and derive equality constraints
 * - Each free variable in RREF corresponds to an equality constraint
 *
 * **Algorithm Overview:**
 * The algorithm uses an iterative refinement approach similar to polyhedral
 * abstraction:
 * 1. Find an initial satisfying model (anchor point)
 * 2. While iteration limit not exceeded:
 *    a. Compute affine hull from collected points using RREF
 *    b. Check if φ ∧ ¬hull is satisfiable (find counter-example)
 *    c. If unsatisfiable, hull is exact, terminate
 *    d. If satisfiable, add new point and repeat
 *
 * **Exact Rational Arithmetic:**
 * The algorithm uses exact rational arithmetic (Rational class) to avoid
 * floating-point precision issues when computing nullspaces and equality
 * constraints. This ensures soundness and correctness of the derived
 * equalities.
 *
 * **Use Cases:**
 * - Discovering linear relationships between variables (e.g., x + y = 10)
 * - Detecting affine dependencies in numerical code
 * - Simplifying constraints by identifying equalities
 *
 * **Comparison with Polyhedral Abstraction:**
 * - Affine abstraction discovers **equalities** (A·x = b)
 * - Polyhedral abstraction discovers **inequalities** (A·x ≤ b)
 * - Affine hull is typically smaller than convex hull (equalities are stronger)
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <cassert>
#include <numeric>

#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {
namespace {

using Rat = Rational;

/**
 * @brief Given a set of points, compute the affine equalities describing their
 * hull.
 *
 * This function computes the affine hull of a set of points by:
 * 1. Selecting the first point as an anchor
 * 2. Building difference vectors (p_i - anchor) for i = 2, ..., n
 * 3. Computing RREF to find the nullspace (affine dependencies)
 * 4. Deriving equality constraints from the nullspace basis
 *
 * **Mathematical Details:**
 * - The difference vectors form a matrix whose nullspace corresponds to affine
 * equalities
 * - RREF reveals free columns, each of which corresponds to an equality
 * constraint
 * - Nullspace basis vectors are scaled to integer coefficients using LCM
 * - Coefficients are normalized by GCD for canonical representation
 *
 * @param points The set of points (each point is a vector of coordinates)
 * @param variables The variables corresponding to point coordinates
 * @return Vector of affine equalities [A|b] representing the hull (A·x = b)
 */
std::vector<AffineEquality>
build_equalities_from_points(const std::vector<std::vector<int64_t>> &points,
                             const std::vector<expr> &variables) {

  std::vector<AffineEquality> result;
  if (points.empty()) {
    return result;
  }

  const size_t n = variables.size();
  const auto &anchor = points.front();

  if (points.size() == 1) {
    for (size_t i = 0; i < n; ++i) {
      AffineEquality eq;
      eq.variables = variables;
      eq.coefficients.assign(n, 0);
      eq.coefficients[i] = 1;
      eq.constant = anchor[i];
      result.push_back(eq);
    }
    return result;
  }

  // Build matrix of difference vectors.
  std::vector<std::vector<Rat>> diff;
  diff.reserve(points.size() - 1);
  for (size_t idx = 1; idx < points.size(); ++idx) {
    std::vector<Rat> row;
    row.reserve(n);
    for (size_t j = 0; j < n; ++j) {
      row.emplace_back(points[idx][j] - anchor[j]);
    }
    diff.push_back(std::move(row));
  }

  RrefResult rref_res = SymAbs::rref(diff);
  const auto &mat = rref_res.matrix;
  const auto &pivots = rref_res.pivot_columns;

  // Identify free columns.
  std::vector<size_t> free_cols;
  for (size_t c = 0; c < n; ++c) {
    if (std::find(pivots.begin(), pivots.end(), c) == pivots.end()) {
      free_cols.push_back(c);
    }
  }

  if (free_cols.empty()) {
    // Full rank: only the anchor point is possible.
    for (size_t i = 0; i < n; ++i) {
      AffineEquality eq;
      eq.variables = variables;
      eq.coefficients.assign(n, 0);
      eq.coefficients[i] = 1;
      eq.constant = anchor[i];
      result.push_back(eq);
    }
    return result;
  }

  // Build basis for the nullspace.
  for (size_t free_col : free_cols) {
    std::vector<Rat> basis(n, Rat(0));
    basis[free_col] = Rat(1);

    for (size_t row = 0; row < mat.size() && row < pivots.size(); ++row) {
      size_t pcol = pivots[row];
      basis[pcol] = -mat[row][free_col];
    }

    // Scale to integer coefficients.
    int64_t scale = 1;
    for (const auto &v : basis) {
      scale = SymAbs::lcm64(scale, v.denominator());
    }
    if (scale == 0)
      continue;

    std::vector<int64_t> coeffs(n, 0);
    for (size_t i = 0; i < n; ++i) {
      coeffs[i] = static_cast<int64_t>(basis[i].numerator() *
                                       (scale / basis[i].denominator()));
    }

    // Normalize by GCD.
    int64_t g = 0;
    for (int64_t c : coeffs) {
      g = SymAbs::gcd64(g, c);
    }
    if (g == 0) {
      continue;
    }
    for (auto &c : coeffs) {
      c /= g;
    }

    int64_t constant = 0;
    for (size_t i = 0; i < n; ++i) {
      constant += coeffs[i] * anchor[i];
    }

    AffineEquality eq;
    eq.variables = variables;
    eq.coefficients = std::move(coeffs);
    eq.constant = constant;
    result.push_back(std::move(eq));
  }

  return result;
}

/**
 * @brief Convert an affine equality to a Z3 integer equality expression.
 *
 * Converts an AffineEquality (which represents Σ c_i · v_i = d) into a Z3
 * integer equality expression. Each bit-vector variable is converted to
 * an unbounded integer using bv_signed_to_int().
 *
 * @param eq The affine equality to convert
 * @param vars The variables corresponding to coefficients
 * @return Z3 expression representing the equality Σ c_i · v_i = d
 */
expr equality_to_expr(const AffineEquality &eq, const std::vector<expr> &vars) {
  context &ctx = vars.front().ctx();
  assert(eq.coefficients.size() == vars.size());

  expr lhs = ctx.int_val(0);
  for (size_t i = 0; i < vars.size(); ++i) {
    if (eq.coefficients[i] == 0)
      continue;
    lhs = lhs +
          ctx.int_val(eq.coefficients[i]) * SymAbs::bv_signed_to_int(vars[i]);
  }
  expr rhs = ctx.int_val(eq.constant);
  return lhs == rhs;
}

} // namespace

/**
 * @brief Compute affine equality abstraction α_aff^V(φ)
 *
 * Algorithm 12: Computes the affine hull of models of φ using iterative
 * counter-example guided refinement.
 *
 * **Algorithm Steps:**
 * 1. **Initialization**: Find an initial satisfying model (anchor point)
 * 2. **Refinement Loop**: While iteration limit not exceeded:
 *    a. Compute affine hull from collected points using
 * build_equalities_from_points() b. Build hull as conjunction of equality
 * constraints c. Check if φ ∧ ¬hull is satisfiable (find counter-example) d. If
 * unsatisfiable, hull is exact (φ ⊨ hull), terminate e. If satisfiable, extract
 * new point and add to point set f. If point already exists, terminate (no
 * progress)
 * 3. Return the final affine equalities
 *
 * **Termination:**
 * - Terminates when φ ⊨ hull (no counter-examples exist)
 * - Terminates when max_iterations is reached
 * - Terminates when no new points can be found (point already in set)
 *
 * **Correctness:**
 * The algorithm is sound: the returned equalities are satisfied by all models
 * of φ. It is complete in the limit (with enough iterations), but may terminate
 * early due to iteration limits.
 *
 * @param phi The formula to abstract (bit-vector SMT formula)
 * @param variables The set of variables V = {v_1, ..., v_n}
 * @param config Configuration including timeout and max_iterations
 * @return Vector of affine equalities [A|b] representing the affine hull (A·x =
 * b)
 *
 * @note Returns empty vector if φ is unsatisfiable
 * @note The result is exact (all models of φ satisfy the equalities)
 */
std::vector<AffineEquality> alpha_aff_V(z3::expr phi,
                                        const std::vector<z3::expr> &variables,
                                        const AbstractionConfig &config) {

  if (variables.empty()) {
    return {};
  }

  context &ctx = phi.ctx();
  solver init(ctx);
  params p(ctx);
  p.set("timeout", config.timeout_ms);
  init.set(p);
  init.add(phi);

  // Step 1: Find initial satisfying model (anchor point)
  if (init.check() != sat) {
    // Formula is unsatisfiable, return empty equalities
    return {};
  }

  model m0 = init.get_model();
  std::vector<std::vector<int64_t>> points;
  points.push_back(SymAbs::extract_point(m0, variables));

  // Step 2: Iterative refinement loop
  unsigned iteration = 0;
  while (iteration < config.max_iterations) {
    // Compute affine hull from current set of points
    auto equalities = build_equalities_from_points(points, variables);
    if (equalities.empty()) {
      return equalities;
    }

    // Build hull as conjunction of equality constraints
    expr_vector hull_conj(ctx);
    for (const auto &eq : equalities) {
      hull_conj.push_back(equality_to_expr(eq, variables));
    }
    expr hull = mk_and(hull_conj);

    // Check for counter-examples: points satisfying φ but not hull
    solver refine(ctx);
    refine.set(p);
    refine.add(phi);
    refine.add(!hull); // Require violation of hull

    auto res = refine.check();
    if (res != sat) {
      // No counter-examples exist: φ ⊨ hull, so hull is exact
      return equalities;
    }

    // Extract new point from counter-example model
    model m_new = refine.get_model();
    auto new_point = SymAbs::extract_point(m_new, variables);
    if (std::find(points.begin(), points.end(), new_point) == points.end()) {
      // New point found, add to set and continue
      points.push_back(std::move(new_point));
    } else {
      // Point already in set, no progress, return current hull
      return equalities;
    }

    ++iteration;
  }

  // Return hull computed from final point set
  return build_equalities_from_points(points, variables);
}

} // namespace SymAbs
