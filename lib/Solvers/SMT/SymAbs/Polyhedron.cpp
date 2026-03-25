/**
 * @file Polyhedron.cpp
 * @brief Implementation of Algorithm 9: α_conv^V and Algorithm 10: relax-conv
 *
 * This module implements convex polyhedral abstraction for computing
 * over-approximations of bit-vector formulas using convex polyhedra
 * (conjunctions of linear inequalities).
 *
 * **Convex Polyhedral Domain:**
 * The polyhedral domain represents sets of points using linear inequalities of
 * the form: Σ c_i · v_i ≤ d where c_i are integer coefficients and d is a
 * constant. This is the most expressive numerical abstract domain among zones,
 * octagons, and polyhedra, allowing arbitrary linear combinations of variables.
 *
 * **Algorithm 9: α_conv^V(φ)**
 * Computes a convex polyhedron over-approximating φ by iteratively:
 * 1. Finding extremal points (vertices) that satisfy φ but violate the current
 * polyhedron
 * 2. Computing the convex hull of all discovered points
 * 3. Building supporting half-spaces from the convex hull
 * 4. Terminating when no violating points exist or iteration limits are reached
 *
 * **Algorithm 10: relax-conv(φ, c)**
 * Relaxes a template polyhedron c by computing optimal bounds for each template
 * constraint using α_lin-exp. This is more efficient than Algorithm 9 when
 * template constraints are known in advance.
 *
 * **Key Components:**
 * - **Convex Hull Computation**: Builds half-spaces from vertices using facet
 * enumeration
 * - **Facet Discovery**: Identifies supporting hyperplanes by computing normals
 * from subsets of n points (where n is the dimension)
 * - **Iterative Refinement**: Uses counter-example guided abstraction
 * refinement (CEGAR) to improve precision
 *
 * **Complexity:**
 * - Facet enumeration is exponential in the worst case (O(n^d) for d
 * dimensions)
 * - Practical performance depends heavily on the number of discovered vertices
 * - Early termination via max_iterations and max_inequalities limits prevents
 * explosion
 *
 * **Use Cases:**
 * - Precise numerical invariant discovery
 * - Relationship discovery between multiple variables
 * - Overflow and array bounds analysis when octagons/zones are insufficient
 */

#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"
#include "Verification/SymAbsAI/Utils/Z3APIExtension.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <sstream>
#include <unordered_set>
#include <vector>

#include <z3++.h>
#include <z3.h>

using namespace z3;

namespace SymAbs {
namespace {

/**
 * @brief Build a Z3 inequality expression from an AffineEquality constraint.
 *
 * Converts an AffineEquality (which represents Σ c_i · v_i ≤ d) into a Z3
 * integer inequality expression. Each bit-vector variable is converted to
 * an unbounded integer using bv_signed_to_int().
 *
 * @param ineq The affine inequality to convert
 * @param variables The variables corresponding to coefficients
 * @param ctx The Z3 context
 * @return Z3 expression representing the inequality Σ c_i · v_i ≤ d
 */
static expr build_inequality(const AffineEquality &ineq,
                             const std::vector<expr> &variables, context &ctx) {
  assert(ineq.coefficients.size() == variables.size());
  expr lhs = ctx.int_val(0);
  for (size_t i = 0; i < variables.size(); ++i) {
    if (ineq.coefficients[i] == 0)
      continue;
    lhs = lhs + ctx.int_val(ineq.coefficients[i]) *
                    SymAbs::bv_signed_to_int(variables[i]);
  }
  expr rhs = ctx.int_val(ineq.constant);
  return lhs <= rhs;
}

/**
 * @brief Build the negation of a polyhedron (disjunction of constraint
 * violations).
 *
 * For a polyhedron represented as a conjunction of inequalities, the negation
 * is the disjunction of negated inequalities. This is used in Algorithm 9 to
 * find points that violate the current polyhedron approximation.
 *
 * **Mathematical relationship:**
 *   ¬(c_1 ∧ c_2 ∧ ... ∧ c_n) = (¬c_1) ∨ (¬c_2) ∨ ... ∨ (¬c_n)
 *
 * @param poly The polyhedron (vector of affine inequalities)
 * @param variables The variables used in the inequalities
 * @param ctx The Z3 context
 * @return Z3 expression representing ¬poly (disjunction of violated
 * constraints)
 */
static expr build_not_poly(const std::vector<AffineEquality> &poly,
                           const std::vector<expr> &variables, context &ctx) {
  if (poly.empty()) {
    return ctx.bool_val(true);
  }

  expr_vector violations(ctx);
  for (const auto &ineq : poly) {
    violations.push_back(!build_inequality(ineq, variables, ctx));
  }

  return mk_or(violations);
}

/**
 * @brief Compute a candidate facet normal vector from a subset of n points.
 *
 * Given n points in n-dimensional space that are affinely independent (i.e.,
 * they form a facet), this function computes the normal vector of the
 * hyperplane containing these points. The normal vector defines the half-space
 * inequality.
 *
 * **Mathematical Background:**
 * - If n points p_1, ..., p_n define a facet, then the vectors (p_i - p_1) for
 *   i = 2, ..., n span an (n-1)-dimensional subspace
 * - The nullspace of this subspace (computed via RREF) has dimension 1
 * - A basis vector of the nullspace is the normal to the facet
 * - The normal is scaled to integer coefficients using LCM
 *
 * **Algorithm:**
 * 1. Build difference vectors matrix: [p_2 - p_1; p_3 - p_1; ...; p_n - p_1]
 * 2. Compute RREF to find the nullspace
 * 3. Extract the basis vector (normal) from the nullspace
 * 4. Scale to integer coefficients using LCM of denominators
 *
 * @param subset Vector of n points (each point is a vector of n coordinates)
 * @return Normal vector as integer coefficients, or None if points don't define
 * a facet
 *
 * @note Returns None if subset is empty or size doesn't match dimension
 * @note Returns None if nullspace dimension ≠ 1 (points don't define a unique
 * facet)
 */
static llvm::Optional<std::vector<int64_t>>
compute_normal_from_subset(const std::vector<std::vector<int64_t>> &subset) {
  if (subset.empty())
    return llvm::None;
  const size_t n = subset.front().size();
  if (subset.size() != n) {
    return llvm::None;
  }

  // Build matrix of difference vectors (n-1 rows, n columns).
  // Each row represents a difference vector (p_i - p_1) for i = 2, ..., n
  std::vector<std::vector<Rational>> diff;
  diff.reserve(n - 1);
  for (size_t i = 1; i < subset.size(); ++i) {
    std::vector<Rational> row;
    row.reserve(n);
    for (size_t j = 0; j < n; ++j) {
      row.emplace_back(subset[i][j] - subset[0][j]);
    }
    diff.push_back(std::move(row));
  }

  // Compute reduced row echelon form to find the nullspace
  RrefResult rr = SymAbs::rref(diff);
  const auto &mat = rr.matrix;
  const auto &pivots = rr.pivot_columns;

  // Identify free columns (non-pivot columns, which correspond to nullspace
  // dimensions)
  std::vector<size_t> free_cols;
  for (size_t c = 0; c < n; ++c) {
    if (std::find(pivots.begin(), pivots.end(), c) == pivots.end()) {
      free_cols.push_back(c);
    }
  }

  if (free_cols.size() != 1) {
    // Nullspace dimension not equal to 1 -> not a unique facet.
    // For a valid facet in n dimensions, we need exactly 1 free column.
    return llvm::None;
  }

  // Build basis vector for the nullspace
  size_t free_col = free_cols.front();
  std::vector<Rational> basis(n, Rational(0));
  basis[free_col] = Rational(1); // Set free variable to 1

  // Solve for other variables based on RREF matrix
  for (size_t row = 0; row < mat.size() && row < pivots.size(); ++row) {
    size_t pcol = pivots[row];
    basis[pcol] = -mat[row][free_col];
  }

  // Scale to integer coefficients: find LCM of all denominators
  int64_t scale = 1;
  for (const auto &v : basis) {
    scale = SymAbs::lcm64(scale, v.denominator());
  }
  if (scale == 0) {
    return llvm::None;
  }

  // Convert to integer vector
  std::vector<int64_t> normal(n, 0);
  for (size_t i = 0; i < n; ++i) {
    normal[i] = static_cast<int64_t>(basis[i].numerator() *
                                     (scale / basis[i].denominator()));
  }

  return normal;
}

/**
 * @brief Normalize a normal vector and bound by dividing by GCD.
 *
 * Normalizes a half-space inequality n·x ≤ b by:
 * 1. Dividing all coefficients and bound by GCD(n) to get a normalized
 * representation
 * 2. Ensuring the first non-zero coefficient is positive (standard orientation)
 *
 * **Purpose:**
 * - Eliminates redundancy (multiple representations of same constraint)
 * - Ensures canonical form for constraint comparison
 * - Reduces coefficient size for numerical stability
 *
 * @param normal The normal vector (modified in-place)
 * @param bound The bound value (modified in-place)
 * @return true if normalization succeeded, false if normal is all zeros
 */
static bool normalize(std::vector<int64_t> &normal, int64_t &bound) {
  // Compute GCD of all coefficients
  int64_t g = 0;
  for (auto c : normal) {
    g = SymAbs::gcd64(g, c);
  }
  if (g == 0) {
    // All coefficients are zero, invalid normal
    return false;
  }
  // Divide by GCD to get normalized coefficients
  for (auto &c : normal) {
    c /= g;
  }
  bound /= g;

  // Fix orientation: make the first non-zero coefficient positive.
  // This ensures canonical representation (e.g., always use x ≤ 5, not -x ≤ -5)
  for (auto c : normal) {
    if (c == 0)
      continue;
    if (c < 0) {
      // Negate all coefficients and bound to flip orientation
      for (auto &v : normal)
        v = -v;
      bound = -bound;
    }
    break; // Only check first non-zero coefficient
  }
  return true;
}

/**
 * @brief Compute dot product of two integer vectors.
 *
 * @param a First vector
 * @param b Second vector
 * @return a · b = Σ a[i] * b[i]
 *
 * @pre a.size() == b.size()
 */
static int64_t dot(const std::vector<int64_t> &a,
                   const std::vector<int64_t> &b) {
  assert(a.size() == b.size());
  int64_t res = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    res += a[i] * b[i];
  }
  return res;
}

/**
 * @brief Build supporting half-spaces of the convex hull of the given points.
 *
 * This function implements facet enumeration for computing the convex hull of a
 * set of points. It generates the half-space inequalities (facets) that define
 * the convex hull.
 *
 * **Algorithm:**
 * 1. Add per-variable bounds (axis-aligned bounding box) for efficiency
 * 2. For each combination of n points (where n is dimension):
 *    - Compute candidate facet normal using compute_normal_from_subset()
 *    - Check if normal defines a supporting half-space
 *    - Add normalized constraint if valid
 *
 * **Supporting Half-space Check:**
 * For normal n and bound b computed from points in subset, check:
 * - min_dot = min{n · p | p in all points} ≤ b
 * - max_dot = max{n · p | p in all points} ≥ b
 * - If min_dot == max_dot == b: equality constraint (facet spans all points)
 * - If max_dot == b: half-space n·x ≤ b is supporting
 * - If min_dot == b: half-space -n·x ≤ -b is supporting
 *
 * @param points The set of points to compute convex hull for
 * @param variables The variables corresponding to point coordinates
 * @return Vector of affine inequalities representing the convex hull
 */
static std::vector<AffineEquality>
convex_hull_halfspaces(const std::vector<std::vector<int64_t>> &points,
                       const std::vector<expr> &variables) {
  std::vector<AffineEquality> constraints;
  if (points.empty()) {
    return constraints;
  }

  const size_t n = variables.size();

  // Per-variable bounds keep the hull closed and inexpensive to compute.
  for (size_t i = 0; i < n; ++i) {
    int64_t min_v = points.front()[i];
    int64_t max_v = points.front()[i];
    for (const auto &p : points) {
      min_v = std::min(min_v, p[i]);
      max_v = std::max(max_v, p[i]);
    }
    AffineEquality lower;
    lower.variables = variables;
    lower.coefficients.assign(n, 0);
    lower.coefficients[i] = -1;
    lower.constant = -min_v;
    constraints.push_back(lower);

    AffineEquality upper;
    upper.variables = variables;
    upper.coefficients.assign(n, 0);
    upper.coefficients[i] = 1;
    upper.constant = max_v;
    constraints.push_back(upper);
  }

  if (n == 1 || points.size() == 1) {
    return constraints;
  }

  std::unordered_set<std::string> seen;
  std::vector<size_t> combo;

  auto emit_key = [](const std::vector<int64_t> &norm, int64_t bound) {
    std::ostringstream oss;
    for (auto c : norm) {
      oss << c << ",";
    }
    oss << "|" << bound;
    return oss.str();
  };

  std::function<void(size_t, size_t)> choose = [&](size_t start, size_t need) {
    if (need == 0) {
      std::vector<std::vector<int64_t>> subset;
      subset.reserve(combo.size());
      for (auto idx : combo) {
        subset.push_back(points[idx]);
      }

      auto normal_opt = compute_normal_from_subset(subset);
      if (!normal_opt.hasValue()) {
        return;
      }
      auto normal = normal_opt.getValue();
      int64_t bound = dot(normal, subset.front());
      if (!normalize(normal, bound)) {
        return;
      }

      int64_t min_dot = bound;
      int64_t max_dot = bound;
      for (const auto &p : points) {
        int64_t v = dot(normal, p);
        min_dot = std::min(min_dot, v);
        max_dot = std::max(max_dot, v);
      }

      auto add_constraint = [&](const std::vector<int64_t> &norm, int64_t b) {
        auto key = emit_key(norm, b);
        if (seen.insert(key).second) {
          AffineEquality ineq;
          ineq.variables = variables;
          ineq.coefficients = norm;
          ineq.constant = b;
          constraints.push_back(std::move(ineq));
        }
      };

      if (min_dot == max_dot) {
        add_constraint(normal, bound);
        std::vector<int64_t> neg = normal;
        int64_t neg_b = -bound;
        for (auto &c : neg)
          c = -c;
        add_constraint(neg, neg_b);
      } else if (max_dot <= bound) {
        add_constraint(normal, bound);
      } else if (min_dot >= bound) {
        std::vector<int64_t> neg = normal;
        int64_t neg_b = -bound;
        for (auto &c : neg)
          c = -c;
        add_constraint(neg, neg_b);
      }
      return;
    }

    for (size_t i = start; i + need <= points.size(); ++i) {
      combo.push_back(i);
      choose(i + 1, need - 1);
      combo.pop_back();
    }
  };

  choose(0, n);
  return constraints;
}

} // namespace

/**
 * @brief Compute convex polyhedral abstraction α_conv^V(φ)
 *
 * Algorithm 9: Computes a convex polyhedron over-approximating φ using
 * counter-example guided abstraction refinement (CEGAR).
 *
 * **Algorithm Overview:**
 * The algorithm uses an iterative refinement approach:
 * 1. **Initialization**: Find an initial satisfying model and compute its
 * convex hull
 * 2. **Refinement Loop**: While iteration limit not exceeded:
 *    a. Build ¬polyhedron (disjunction of constraint violations)
 *    b. Check if φ ∧ ¬polyhedron is satisfiable (find counter-example)
 *    c. If unsatisfiable, polyhedron is exact (φ ⊨ polyhedron), terminate
 *    d. If satisfiable, extract extremal points (min/max per variable) that
 * violate the current polyhedron e. Add new points to the point set and
 * recompute convex hull
 * 3. Return the final polyhedron
 *
 * **Extremal Point Extraction:**
 * For each variable v_i, compute min and max values under φ ∧ ¬polyhedron.
 * These extremal values help discover new vertices of the true polyhedron.
 * For each extremal value, attempt to find a satisfying model and extract the
 * full point (all variable values).
 *
 * **Termination:**
 * - Terminates when φ ⊨ polyhedron (no counter-examples exist)
 * - Terminates when max_iterations is reached
 * - Terminates when max_inequalities is reached (to prevent constraint
 * explosion)
 *
 * **Complexity:**
 * - Each iteration may add multiple points and recompute convex hull
 * (exponential in worst case)
 * - Practical performance depends on the number of discovered vertices
 * - Early termination via limits prevents worst-case exponential behavior
 *
 * @param phi The formula to abstract (bit-vector SMT formula)
 * @param variables The set of variables V = {v_1, ..., v_n}
 * @param config Configuration including timeout, max_iterations, and
 * max_inequalities
 * @return Vector of affine inequalities representing the convex polyhedron
 *
 * @note Returns empty vector if φ is unsatisfiable
 * @note The result is an over-approximation (may include points not in φ)
 */
std::vector<AffineEquality> alpha_conv_V(z3::expr phi,
                                         const std::vector<z3::expr> &variables,
                                         const AbstractionConfig &config) {
  if (variables.empty()) {
    return {};
  }

  context &ctx = phi.ctx();
  params p(ctx);
  p.set("timeout", config.timeout_ms);

  // Step 1: Find initial satisfying model
  solver init(ctx);
  init.set(p);
  init.add(phi);
  if (init.check() != sat) {
    // Formula is unsatisfiable, return empty polyhedron
    return {};
  }

  // Extract initial point from model
  std::vector<std::vector<int64_t>> points;
  points.push_back(SymAbs::extract_point(init.get_model(), variables));

  // Compute initial convex hull (just a single point initially, so it's a box)
  std::vector<AffineEquality> polyhedron =
      convex_hull_halfspaces(points, variables);

  // Step 2: Iterative refinement loop
  unsigned iteration = 0;
  while (iteration < config.max_iterations &&
         polyhedron.size() < config.max_inequalities) {
    // Build negation of current polyhedron (points that violate at least one
    // constraint)
    expr not_c = build_not_poly(polyhedron, variables, ctx);

    // Check for counter-examples: points satisfying φ but not polyhedron
    solver witness(ctx);
    witness.set(p);
    witness.add(phi);
    witness.add(not_c); // Require violation of polyhedron
    if (witness.check() != sat) {
      // No counter-examples exist: φ ⊨ polyhedron, so polyhedron is exact
      break;
    }

    // Extract extremal points that violate current polyhedron
    // For each variable, find min and max values under φ ∧ ¬polyhedron
    std::vector<std::vector<int64_t>> new_points;
    for (size_t v_idx = 0; v_idx < variables.size(); ++v_idx) {
      auto min_val = minimum(phi && not_c, variables[v_idx], config);
      auto max_val = maximum(phi && not_c, variables[v_idx], config);

      // Helper to extract full point when a variable has a specific value
      auto add_model_point = [&](int64_t value) {
        solver s(ctx);
        s.set(p);
        s.add(phi);
        s.add(not_c);
        // Fix variable to extremal value
        unsigned bv_size = variables[v_idx].get_sort().bv_size();
        s.add(variables[v_idx] ==
              ctx.bv_val(static_cast<uint64_t>(value), bv_size));
        if (s.check() == sat) {
          // Extract full point (all variable values) from model
          new_points.push_back(SymAbs::extract_point(s.get_model(), variables));
        }
      };

      // Try to extract points at extremal values
      if (min_val.hasValue()) {
        add_model_point(min_val.getValue());
      }
      if (max_val.hasValue()) {
        add_model_point(max_val.getValue());
      }
    }

    // Add new unique points to the point set
    for (const auto &pt : new_points) {
      if (std::find(points.begin(), points.end(), pt) == points.end()) {
        points.push_back(pt);
      }
    }

    // Recompute convex hull with updated point set
    auto updated = convex_hull_halfspaces(points, variables);
    if (!updated.empty()) {
      polyhedron = std::move(updated);
    }

    ++iteration;
  }

  return polyhedron;
}

/**
 * @brief Relax a convex polyhedron using template constraints (Algorithm 10:
 * relax-conv)
 *
 * Algorithm 10: Given a set of template constraints c and formula φ, compute
 * the optimal bounds for each template constraint using α_lin-exp.
 *
 * **Purpose:**
 * This is a more efficient alternative to Algorithm 9 when template constraints
 * are known in advance. Instead of discovering constraints via CEGAR, we
 * directly compute optimal bounds for each template constraint.
 *
 * **Algorithm:**
 * For each template constraint Σ c_i · v_i ≤ d_template:
 * 1. Extract the linear expression Σ c_i · v_i
 * 2. Use α_lin-exp(φ, Σ c_i · v_i) to compute the least upper bound d_opt
 * 3. Replace d_template with d_opt in the result
 *
 * **Advantages:**
 * - More efficient: O(k) linear expression maximizations vs. exponential CEGAR
 * - Predictable: number of constraints equals template size
 * - Well-suited when constraint structure is known
 *
 * **Trade-offs:**
 * - Less precise if template doesn't match actual polyhedron shape
 * - Requires template to be provided
 *
 * @param phi The formula to abstract
 * @param template_constraints The template constraints (coefficients fixed,
 * bounds to optimize)
 * @param config Configuration including timeout and iteration limits
 * @return Vector of affine inequalities with optimized bounds
 *
 * @note Template constraints with coefficients that cannot be bounded are
 * skipped
 */
std::vector<AffineEquality>
relax_conv(z3::expr phi,
           const std::vector<AffineEquality> &template_constraints,
           const AbstractionConfig &config) {
  if (template_constraints.empty()) {
    return {};
  }

  std::vector<AffineEquality> result;
  result.reserve(template_constraints.size());

  // For each template constraint, compute optimal bound
  for (const auto &templ : template_constraints) {
    // Build linear expression from template coefficients
    LinearExpression lexpr;
    lexpr.variables = templ.variables;
    lexpr.coefficients = templ.coefficients;

    // Compute least upper bound using Algorithm 7
    auto bound = alpha_lin_exp(phi, lexpr, config);
    if (!bound.hasValue()) {
      // Cannot compute bound, skip this constraint
      continue;
    }

    // Create relaxed constraint with optimized bound
    AffineEquality relaxed = templ;
    relaxed.constant =
        bound.getValue(); // Replace template bound with optimal bound
    result.push_back(std::move(relaxed));
  }

  return result;
}

} // namespace SymAbs
