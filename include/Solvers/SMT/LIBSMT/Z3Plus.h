/*
 * z3plus.h
 * rainoftime@gmail.com
 *
 * We provide the following APIs
 *  - get_expr_vars(exp, vars)
 *      get all variables of exp and store in vars
 *  - get_vars_difference(vars_a, vars_b)
 *      set differences of vars_a and vars_b
 *  - get_k_models(exp, k)
 *      use the solver to get k models
 *  - get_abstract_interval(pre_cond, query)
 *      get the interval of query, under the condition pre_cond
 *  - get_abstract_interval_as_expr
 *      get the result as a z3 expr
 *  - do_constant_propagation(exp)
 *      use cp to simplify exp
 *  - check_model
 *  - solve_with_truth_table
 *
 * Fixes applied:
 *  CC-1 – Removed "using namespace std" and "using namespace z3" from global
 *          scope.  They polluted every translation unit that included this
 *          header.  All identifiers are now fully qualified.
 *  CC-2 – get_abstract_interval(): the maximize result must be read with
 *          upper() and the minimize result with lower() (they were swapped).
 *          Same fix applied to get_abstract_interval_as_expr().
 *  CC-3 – get_k_models(): use get_numeral_uint64() (not get_numeral_int())
 *          to avoid truncation / exception for bit-vectors wider than 31 bits.
 *          The blocking clause now uses the string representation so that
 *          arbitrarily wide bit-vectors are handled correctly.
 */
#pragma once

#include "z3++.h"
#include "z3.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <vector>

// Fix CC-1: do NOT place "using namespace std" or "using namespace z3" here.
// Each .cpp file that needs these namespaces should declare them locally.

inline bool get_expr_vars(z3::expr &exp, z3::expr_vector &vars) {
  /*
   * get the variables in `exp` and put them in `vars`
   */
  try {
    auto &ctx = exp.ctx();
    auto compare_func = [](const z3::expr &a, const z3::expr &b) {
      Z3_symbol sym_a = a.decl().name();
      Z3_symbol sym_b = b.decl().name();
      return sym_a < sym_b;
    };
    std::set<z3::expr, decltype(compare_func)> syms(compare_func);
    std::function<void(const z3::expr &)> recur = [&recur, &syms,
                                                   &ctx](const z3::expr &e) {
      assert(Z3_is_app(ctx, e));
      auto app = Z3_to_app(ctx, e);
      unsigned n_args = Z3_get_app_num_args(ctx, app);

      auto fdecl = Z3_get_app_decl(ctx, app);
      if (n_args == 0 && Z3_get_decl_kind(ctx, fdecl) == Z3_OP_UNINTERPRETED)
        syms.insert(e);

      for (unsigned i = 0; i < n_args; ++i) {
        z3::expr arg(ctx, Z3_get_app_arg(ctx, app, i));
        recur(arg);
      }
    };
    recur(exp);
    for (auto &i : syms) {
      vars.push_back(i);
    }
  } catch (z3::exception &ex) {
    std::cout << ex.msg() << "\n";
    return false;
  }
  return true;
}

inline z3::expr_vector get_vars_difference(z3::expr_vector &vars_a,
                                           z3::expr_vector &vars_b) {
  /*
   * Compute the set difference of vars_a and vars_b.
   * Assumes vars_a and vars_b consist of purely variables.
   */
  z3::expr_vector ret(vars_a.ctx());
  try {
    for (unsigned i = 0; i < vars_a.size(); i++) {
      bool is_in_diff = true;
      Z3_symbol sym_i = vars_a[i].decl().name();
      for (unsigned j = 0; j < vars_b.size(); j++) {
        if (sym_i == vars_b[j].decl().name()) {
          is_in_diff = false;
          break;
        }
      }
      if (is_in_diff) {
        ret.push_back(vars_a[i]);
      }
    }
  } catch (z3::exception &ex) {
    std::cout << ex.msg() << "\n";
    return ret;
  }
  return ret;
}

inline void get_k_models(z3::expr &exp, int k) {
  /*
   * Compute k models of exp.
   *
   * Fix CC-3: use the string representation of the numeral to build the
   * blocking clause so that bit-vectors of any width are handled correctly.
   * The old code used get_numeral_int() which throws / truncates for BVs
   * wider than 31 bits.
   */
  z3::context &ctx = exp.ctx();
  z3::solver solver(ctx);
  solver.add(exp);
  while (solver.check() == z3::sat && k >= 1) {
    std::cout << solver << "\n";
    z3::model m = solver.get_model();
    z3::expr_vector args(ctx);
    for (unsigned i = 0; i < m.size(); i++) {
      z3::func_decl z3Variable = m[i];
      std::string varName = z3Variable.name().str();
      z3::expr val = m.get_const_interp(z3Variable);
      if (val.get_sort().is_bv()) {
        unsigned bvSize = val.get_sort().bv_size();
        // Fix CC-3: use the string numeral to avoid int truncation.
        std::string svalue =
            Z3_get_numeral_string(ctx, static_cast<Z3_ast>(val));
        args.push_back(ctx.bv_const(varName.c_str(), bvSize) !=
                       ctx.bv_val(svalue.c_str(), bvSize));
      }
    }
    if (args.empty())
      break; // no BV variables to block; avoid infinite loop
    solver.add(z3::mk_or(args));
    k--;
  }
}

inline std::pair<int, int> get_abstract_interval(z3::expr &pre_cond,
                                                 z3::expr &query, int timeout) {
  /*
   * Compute the interval abstraction of pre_cond for query.
   *
   * Fix CC-2: after maximize(query), the optimal value is read with upper();
   *           after minimize(query), the optimal value is read with lower().
   *           The original code had these swapped.
   */
  z3::context &c = pre_cond.ctx();
  std::pair<int, int> ret(INT_MIN, INT_MAX);
  z3::optimize opt_max(c);
  z3::optimize opt_min(c);
  z3::params p(c);
  p.set("priority", c.str_symbol("pareto"));
  z3::set_param("smt.timeout", timeout);
  opt_max.set(p);
  opt_min.set(p);

  opt_max.add(pre_cond);
  z3::optimize::handle h_max = opt_max.maximize(query);

  opt_min.add(pre_cond);
  z3::optimize::handle h_min = opt_min.minimize(query);

  try {
    if (opt_max.check() == z3::sat) {
      // Fix CC-2: upper() gives the optimal maximum value.
      ret.second = opt_max.upper(h_max).get_numeral_int();
    }
  } catch (z3::exception &ex) {
    std::cout << ex << "\n";
  }
  try {
    if (opt_min.check() == z3::sat) {
      // Fix CC-2: lower() gives the optimal minimum value.
      ret.first = opt_min.lower(h_min).get_numeral_int();
    }
  } catch (z3::exception &ex) {
    std::cout << ex << "\n";
  }
  return ret;
}

inline void get_abstract_interval_as_expr(z3::expr &pre_cond, z3::expr &query,
                                          z3::expr_vector &res, int timeout) {
  /*
   * Compute the interval abstraction of pre_cond for query, returning
   * Z3 expressions for the lower and upper bounds.
   *
   * Fix CC-2: lower() for the minimum result, upper() for the maximum result.
   */
  z3::context &Ctx = pre_cond.ctx();
  z3::params Param(Ctx);
  Param.set("priority", Ctx.str_symbol("pareto"));
  z3::set_param("smt.timeout", timeout);

  z3::optimize UpperFinder(Ctx);
  z3::optimize LowerFinder(Ctx);
  UpperFinder.set(Param);
  LowerFinder.set(Param);

  UpperFinder.add(pre_cond);
  z3::optimize::handle UpperGoal = UpperFinder.maximize(query);

  LowerFinder.add(pre_cond);
  z3::optimize::handle LowerGoal = LowerFinder.minimize(query);

  try {
    if (LowerFinder.check() == z3::sat) {
      // Fix CC-2: lower() gives the optimal minimum value.
      res.push_back(LowerFinder.lower(LowerGoal));
    }
  } catch (z3::exception &) {
    res.push_back(Ctx.bool_val(false));
  }
  try {
    if (UpperFinder.check() == z3::sat) {
      // Fix CC-2: upper() gives the optimal maximum value.
      res.push_back(UpperFinder.upper(UpperGoal));
    }
  } catch (z3::exception &) {
    res.push_back(Ctx.bool_val(false));
  }
}

inline z3::expr do_constant_propagation(z3::expr &to_simp) {
  /*
   * Perform constant propagation on to_simp.
   */
  z3::goal gg(to_simp.ctx());
  gg.add(to_simp);
  z3::tactic cp = z3::tactic(to_simp.ctx(), "propagate-values");
  return cp.apply(gg)[0].as_expr();
}

inline bool check_model_misc(z3::expr &exp, z3::context &ctx,
                             std::vector<z3::func_decl> &decls,
                             std::vector<int> &candidate) {
  z3::model cur_model(ctx);
  for (unsigned i = 0; i < decls.size(); i++) {
    z3::expr var_i = ctx.bv_val(candidate[i], 32);
    cur_model.add_const_interp(decls[i], var_i);
  }
  return cur_model.eval(exp).is_true();
}

inline bool check_model_with_mutate(z3::expr &exp) {
  z3::expr_vector vars(exp.ctx());
  get_expr_vars(exp, vars);
  unsigned var_num = vars.size();

  std::vector<z3::func_decl> decls;
  for (unsigned i = 0; i < var_num; i++)
    decls.push_back(vars[i].decl());

  std::vector<int> candidate(var_num, 0);
  return check_model_misc(exp, exp.ctx(), decls, candidate);
}

inline bool sat_under_partial_model(z3::expr &exp, z3::model &m,
                                    z3::expr_vector &donot_cared_vars) {
  z3::model partial_model(exp.ctx());
  unsigned num_constants = m.num_consts();
  for (unsigned i = 0; i < num_constants; i++) {
    z3::func_decl decl = m.get_const_decl(i);
    bool add_to_partial_model = true;
    for (unsigned j = 0; j < donot_cared_vars.size(); j++) {
      if (donot_cared_vars[j].decl().name() == decl.name()) {
        add_to_partial_model = false;
        break;
      }
    }
    if (add_to_partial_model) {
      z3::expr val_e = m.get_const_interp(decl);
      partial_model.add_const_interp(decl, val_e);
    }
  }
  return partial_model.eval(exp, true).is_true();
}

inline uint64_t snoob(uint64_t sub, uint64_t set) {
  // from hacker's delight
  uint64_t tmp = sub - 1;
  uint64_t rip = set & (tmp + (sub & (0 - sub)) - set);
  for (sub = (tmp & sub) ^ rip; sub &= sub - 1; rip ^= tmp, set ^= tmp)
    tmp = set & (0 - set);
  return rip;
}

inline bool check_model(z3::expr &exp, z3::context &ctx,
                        std::vector<z3::func_decl> &decls, uint64_t x,
                        unsigned num) {
  z3::model m(ctx);
  z3::expr bfalse = ctx.bool_val(false);
  z3::expr btrue = ctx.bool_val(true);
  for (unsigned i = 0; i < num; i++) {
    if (x & 1)
      m.add_const_interp(decls[i], btrue);
    else
      m.add_const_interp(decls[i], bfalse);
    x >>= 1;
  }
  return m.eval(exp).is_true();
}

inline int solve_with_truth_table(z3::expr &exp, int bound) {
  /*
   * Solve expr with truth-table based brute-force enumeration.
   */
  z3::expr_vector vars(exp.ctx());
  get_expr_vars(exp, vars);
  unsigned var_num = vars.size();
  if (var_num > 15)
    return 2; // unknown

  int ret = 2;
  std::vector<z3::func_decl> decls;
  for (unsigned i = 0; i < var_num; i++)
    decls.push_back(vars[i].decl());

  auto set = (1ULL << var_num) - 1;
  int check_model_count = 0;
  for (unsigned i = 0; i < var_num + 1; ++i) {
    auto sub = (1ULL << i) - 1;
    uint64_t x = sub;
    uint64_t y;
    do {
      check_model_count++;
      if (check_model(exp, exp.ctx(), decls, x, var_num)) {
        ret = 1;
        goto RET;
      }
      if (check_model_count > bound)
        goto RET;
      y = snoob(x, set);
      x = y;
    } while (y > sub);
  }
  ret = 0;
RET:
  return ret;
}
