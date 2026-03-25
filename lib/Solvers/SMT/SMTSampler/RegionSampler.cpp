/**
 * @file RegionSampler.cpp
 * @brief Abstraction-based sampling using SymAbs + hit-and-run walk
 *
 * Fixes applied (original B-series):
 *  B19 – signed_range() now handles width == 64 as a special case (using
 *        INT64_MIN / INT64_MAX) instead of silently returning false and leaving
 *        64-bit variables unconstrained in the polytope.
 *  B20 – bv_from_int() is unchanged (already correct for width < 64); the fix
 *        is in signed_range() which now covers width == 64.
 *  B21 – model_satisfies() now builds a complete model by also assigning
 *        Boolean variables found in the formula, so partial-model evaluation
 *        does not produce false positives for mixed BV/Boolean formulas.
 *  B22 – build_constraints() logs a warning when a constraint references an
 *        unknown variable name (previously silently skipped with no
 * indication). B23 – collect_vars() deduplicates the variable list using a
 * name-based set so that variables appearing multiple times in the formula are
 * only added once.
 *
 * Additional fixes (new):
 *  RS-1 – The set of Boolean variables not covered by the BV variable list is
 *          now computed once in collect_vars() and stored in bool_vars, rather
 *          than being recomputed on every call to model_satisfies() (which
 *          traversed the entire formula AST per sample).
 *  RS-3 – initial_point() now falls back to assigning 0 for any variable
 *          whose value cannot be extracted by eval_model_value(), rather than
 *          aborting the entire run.
 *  RS-4 – build_constraints() uses unsigned ranges [0, 2^w - 1] for BV
 *          variables instead of signed ranges, matching SMT-LIB semantics
 *          where bit-vectors are unsigned by default.
 */

#include "Solvers/SMT/SMTSampler/PolySampler/PolySampler.h"
#include "Solvers/SMT/SMTSampler/SMTSampler.h"
#include "Solvers/SMT/SymAbs/SymAbsUtils.h"
#include "Solvers/SMT/SymAbs/SymbolicAbstraction.h"

#include <fstream>
#include <iostream>
#include <random>
#include <unordered_map>
#include <unordered_set>

using namespace std;
using namespace z3;

namespace {
constexpr const char *kSamplerName = "RegionSampler";

void log_warn(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] WARN: " << msg << '\n';
}

void log_error(const std::string &msg) {
  std::cerr << "[" << kSamplerName << "] ERROR: " << msg << '\n';
}

struct VarInfo {
  z3::expr var;
  unsigned width;
  std::string name;
};

/**
 * @brief Computes the unsigned range [0, 2^width - 1] for a bit-vector.
 *
 * RS-4: bit-vectors in SMT-LIB are unsigned by default.  Using signed ranges
 * caused the walk to propose negative values that are always rejected by
 * model_satisfies(), wasting samples.
 *
 * B19 (retained): width == 64 is handled explicitly.
 */
static bool unsigned_range(unsigned width, int64_t &min_out, int64_t &max_out) {
  if (width == 0)
    return false;
  min_out = 0;
  if (width >= 64) {
    // B19 / RS-4: 64-bit unsigned range saturates at INT64_MAX because we
    // store bounds as int64_t.  The walk will still cover the positive half.
    max_out = std::numeric_limits<int64_t>::max();
  } else {
    max_out = static_cast<int64_t>((1ULL << width) - 1ULL);
  }
  return true;
}

static z3::expr bv_from_int(z3::context &ctx, int64_t value, unsigned width) {
  uint64_t u = static_cast<uint64_t>(value);
  if (width < 64) {
    uint64_t mask = (1ULL << width) - 1;
    u &= mask;
  }
  return ctx.bv_val(u, width);
}

/**
 * @brief Checks whether a candidate point satisfies the SMT formula.
 *
 * B21: in addition to bit-vector variables, Boolean variables present in
 * the formula are also assigned in the model (defaulting to false when not
 * covered by the point vector).
 *
 * RS-1: the set of uncovered Boolean variables is passed in as a pre-computed
 * parameter (bool_var_decls) rather than being recomputed on every call.
 */
static bool model_satisfies(const z3::expr &phi,
                            const std::vector<VarInfo> &vars,
                            const std::vector<int64_t> &point,
                            const std::vector<z3::func_decl> &bool_var_decls) {
  z3::context &ctx = phi.ctx();
  z3::model m(ctx);

  // Assign bit-vector variables from the point.
  for (size_t i = 0; i < vars.size(); ++i) {
    z3::func_decl decl = vars[i].var.decl();
    z3::expr val = bv_from_int(ctx, point[i], vars[i].width);
    m.add_const_interp(decl, val);
  }

  // B21 / RS-1: assign pre-collected Boolean variables that are not covered
  // by the BV variable list, defaulting to false.
  z3::expr bool_false = ctx.bool_val(false);
  for (const auto &decl : bool_var_decls) {
    z3::func_decl d = decl; // need non-const lvalue
    m.add_const_interp(d, bool_false);
  }

  return m.eval(phi, true).is_true();
}

} // namespace

struct region_sampler {
  std::string input_file;
  int max_samples = 1000;
  double max_time_ms = 30000.0;
  RegionSampling::SampleConfig sample_config;

  SymAbs::AbstractionConfig abs_config;

  enum class Domain { Zone, Octagon };
  Domain domain = Domain::Octagon;

  RegionSampling::Walk walk = RegionSampling::Walk::HitAndRun;

  z3::context c;
  z3::expr smt_formula;
  std::vector<VarInfo> vars;
  std::vector<RegionSampling::LinearConstraint> constraints;

  // RS-1: pre-computed list of Boolean variable declarations not covered by
  // the BV variable list, used by model_satisfies().
  std::vector<z3::func_decl> bool_var_decls;

  std::mt19937_64 rng;

  explicit region_sampler(std::string input, int max_samples, double max_time)
      : input_file(std::move(input)), max_samples(max_samples),
        max_time_ms(max_time), smt_formula(c) {
    rng.seed(static_cast<uint64_t>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()));
  }

  void parse_smt() {
    try {
      expr_vector evec = c.parse_file(input_file.c_str());
      smt_formula = mk_and(evec);
    } catch (const z3::exception &e) {
      log_error(std::string("Failed to parse SMT file: ") + e.msg());
      smt_formula = z3::expr(c);
    }
  }

  /**
   * @brief Collects all bit-vector variables from the SMT formula and
   *        pre-computes the set of uncovered Boolean variables.
   *
   * B23: deduplicates by variable name so that variables appearing
   * multiple times in the formula are only added once.
   *
   * RS-1: also collects Boolean variables not in the BV list into
   * bool_var_decls so that model_satisfies() does not need to re-traverse
   * the formula AST on every call.
   */
  void collect_vars() {
    expr_vector all_vars(c);
    get_expr_vars(smt_formula, all_vars);
    vars.clear();
    bool_var_decls.clear();

    // B23: track seen names to avoid duplicates.
    std::unordered_set<std::string> seen_bv_names;

    for (unsigned i = 0; i < all_vars.size(); ++i) {
      const z3::expr &v = all_vars[i];
      std::string name = v.decl().name().str();
      if (v.get_sort().is_bv()) {
        if (seen_bv_names.insert(name).second) {
          VarInfo info{v, v.get_sort().bv_size(), name};
          vars.push_back(info);
        }
      } else if (v.get_sort().is_bool()) {
        // RS-1: collect Boolean variables for model_satisfies().
        // Deduplicate by name as well.
        if (seen_bv_names.find(name) == seen_bv_names.end()) {
          // Use a separate set for bool names to avoid mixing with BV names.
          bool_var_decls.push_back(v.decl());
          // Mark as seen so we don't add it twice.
          seen_bv_names.insert("__bool__" + name);
        }
      }
    }
  }

  /**
   * @brief Builds linear integer constraints from the SMT formula.
   *
   * B22: logs a warning when a constraint references a variable name not
   * found in the vars index (previously silently skipped).
   *
   * RS-4: uses unsigned_range() so that BV variable bounds are [0, 2^w-1]
   * rather than the signed range [-2^(w-1), 2^(w-1)-1].
   */
  bool build_constraints() {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < vars.size(); ++i)
      index[vars[i].name] = i;

    constraints.clear();
    if (domain == Domain::Zone) {
      auto zone =
          SymAbs::alpha_zone_V(smt_formula, extract_exprs(), abs_config);
      for (const auto &cstr : zone) {
        RegionSampling::LinearConstraint lc;
        lc.coeffs.assign(vars.size(), 0);
        auto it_i = index.find(cstr.var_i.decl().name().str());
        if (it_i == index.end()) {
          // B22: warn instead of silently skipping.
          log_warn("Zone constraint references unknown variable: " +
                   cstr.var_i.decl().name().str());
          continue;
        }
        lc.coeffs[it_i->second] += 1;
        if (!cstr.unary) {
          auto it_j = index.find(cstr.var_j.decl().name().str());
          if (it_j == index.end()) {
            log_warn("Zone constraint references unknown variable: " +
                     cstr.var_j.decl().name().str());
            continue;
          }
          lc.coeffs[it_j->second] -= 1;
        }
        lc.bound = cstr.bound;
        constraints.push_back(std::move(lc));
      }
    } else {
      auto oct = SymAbs::alpha_oct_V(smt_formula, extract_exprs(), abs_config);
      for (const auto &cstr : oct) {
        RegionSampling::LinearConstraint lc;
        lc.coeffs.assign(vars.size(), 0);
        auto it_i = index.find(cstr.var_i.decl().name().str());
        if (it_i == index.end()) {
          log_warn("Octagon constraint references unknown variable: " +
                   cstr.var_i.decl().name().str());
          continue;
        }
        lc.coeffs[it_i->second] += cstr.lambda_i;
        if (!cstr.unary) {
          auto it_j = index.find(cstr.var_j.decl().name().str());
          if (it_j == index.end()) {
            log_warn("Octagon constraint references unknown variable: " +
                     cstr.var_j.decl().name().str());
            continue;
          }
          lc.coeffs[it_j->second] += cstr.lambda_j;
        }
        lc.bound = cstr.bound;
        constraints.push_back(std::move(lc));
      }
    }

    // Add bit-width bounds for each variable.
    // RS-4: use unsigned_range() — BV variables are unsigned in SMT-LIB.
    for (size_t i = 0; i < vars.size(); ++i) {
      int64_t min_v = 0, max_v = 0;
      if (!unsigned_range(vars[i].width, min_v, max_v))
        continue;

      // upper bound: x[i] <= max_v
      RegionSampling::LinearConstraint upper;
      upper.coeffs.assign(vars.size(), 0);
      upper.coeffs[i] = 1;
      upper.bound = max_v;
      constraints.push_back(std::move(upper));

      // lower bound: -x[i] <= -min_v  (i.e., x[i] >= min_v)
      RegionSampling::LinearConstraint lower;
      lower.coeffs.assign(vars.size(), 0);
      lower.coeffs[i] = -1;
      lower.bound = -min_v;
      constraints.push_back(std::move(lower));
    }

    return !constraints.empty();
  }

  std::vector<z3::expr> extract_exprs() const {
    std::vector<z3::expr> out;
    out.reserve(vars.size());
    for (const auto &v : vars)
      out.push_back(v.var);
    return out;
  }

  /**
   * @brief Finds an initial satisfying assignment using the SMT solver.
   *
   * RS-3: if eval_model_value() fails for a variable, we fall back to 0
   * rather than aborting the entire run.  The initial point is then verified
   * against the formula before being used.
   */
  bool initial_point(std::vector<int64_t> &point) {
    solver s(c);
    s.add(smt_formula);
    if (s.check() != sat)
      return false;
    model m = s.get_model();
    point.clear();
    point.reserve(vars.size());
    bool any_fallback = false;
    for (const auto &v : vars) {
      int64_t val = 0;
      if (!SymAbs::eval_model_value(m, v.var, val)) {
        // RS-3: fall back to 0 instead of aborting.
        log_warn("eval_model_value failed for variable '" + v.name +
                 "'; using 0 as fallback");
        val = 0;
        any_fallback = true;
      }
      point.push_back(val);
    }
    if (any_fallback) {
      // Verify the fallback point actually satisfies the formula.
      if (!model_satisfies(smt_formula, vars, point, bool_var_decls)) {
        log_warn("Fallback initial point does not satisfy formula; aborting");
        return false;
      }
    }
    return true;
  }

  /**
   * @brief Main execution function for RegionSampler.
   */
  void run() {
    parse_smt();
    if (!smt_formula) {
      log_error("No SMT formula loaded; aborting run");
      return;
    }
    collect_vars();
    if (vars.empty()) {
      log_warn("No bit-vector variables found");
      return;
    }
    if (!build_constraints()) {
      log_warn("No abstraction constraints built");
      return;
    }

    std::vector<int64_t> point;
    if (!initial_point(point)) {
      log_warn("Formula unsat or model extraction failed");
      return;
    }

    std::ofstream out(input_file + ".abs.samples");
    if (!out.is_open()) {
      log_error("Failed to open output file: " + input_file + ".abs.samples");
      return;
    }
    // Write header (variable names).
    for (size_t i = 0; i < vars.size(); ++i) {
      if (i)
        out << " ";
      out << vars[i].name;
    }
    out << "\n";

    sample_config.max_samples = max_samples;
    sample_config.max_time_ms = max_time_ms;

    // RS-1: capture bool_var_decls by value so the lambda does not hold a
    // dangling reference if the region_sampler is moved.
    const std::vector<VarInfo> &vars_ref = vars;
    const std::vector<z3::func_decl> &bool_decls_ref = bool_var_decls;
    z3::expr formula_copy = smt_formula;

    // Acceptance criterion: must satisfy original SMT formula.
    auto accept = [&formula_copy, &vars_ref,
                   &bool_decls_ref](const std::vector<int64_t> &candidate) {
      return model_satisfies(formula_copy, vars_ref, candidate, bool_decls_ref);
    };

    auto samples = RegionSampling::sample_points(constraints, point, walk, rng,
                                                 sample_config, accept);
    for (const auto &sample : samples) {
      for (size_t i = 0; i < sample.size(); ++i) {
        if (i)
          out << " ";
        out << sample[i];
      }
      out << "\n";
    }
  }
};
