/*
 Abstract:
 Abstract interface for OMT solving

 Author:

 rainoftime

 Notes:
 - currently, we only handle single variable interval abstraction


 TODO:
 - it seems hard to set a global timeout
 - identify fixed and unbounded case !!!
 - customize bv_size_reduction tactic to get sound bounds info.
 - signed and unsigned bit-vectors
 - multiple-objectives

 */

#pragma once

#include "z3++.h"
#include "z3.h"

#include <cstdint>
#include <limits>
#include <vector>
using namespace z3;

class optutil {
  unsigned m_timeout = 3000; // three seconds

  bool m_var_lge_zero = true; // we will assume every queried var is >= 0

private:
  void get_k_models(z3::expr &exp, int k);

  bool is_var_fixed(z3::expr &pre_cond, z3::expr &var);

  void partition_iterval(int64_t low, int64_t upp, int64_t k);
};

class opt_solver {
private:
  int m_verbose_lvl = 0;
  bool m_use_ppqf_bv_light = false;
  unsigned m_timeout = 10000; // three seconds
  double m_run_time = 0;      // runtime
  bool m_var_lge_zero = true; // we will assume every queried var is >= 0
  bool m_prune_unsat = true;  // use a first check to prune unsat case

  double m_linear_search_time = 0;
  unsigned m_linear_search_solver_call = 0;
  unsigned m_linear_search_max = 0;

  double m_binary_search_time = 0;
  unsigned m_binary_search_solver_call = 0;
  unsigned m_binary_search_max = 0;

  double m_compact_search_time = 0;
  unsigned m_compact_check_solver_call = 0;
  bool m_compact_use_bound_ana = true;

  double m_z3opt_time = 0;
  unsigned m_z3opt_max = 0;

  double m_qsmt_time = 0; // seems unsound

public:
  // parameters for partitioned search
  typedef enum { disjoint, shared } partition_strategy;

  typedef enum { t_false, t_true, t_undef } tri_bool;

  typedef enum {
    g_min,    // get min
    g_max,    // get max
    g_min_max // get min and max
  } opt_mode;

  opt_solver() {}

  ~opt_solver() {}

  static int64_t get_signed_max(unsigned sz) {
    if (sz == 0) {
      return 0;
    }
    if (sz >= 64) {
      return std::numeric_limits<int64_t>::max();
    }
    return static_cast<int64_t>((1ULL << (sz - 1)) - 1ULL);
  }

  static uint64_t get_unsigned_max(unsigned sz) {
    if (sz == 0) {
      return 0;
    }
    if (sz >= 64) {
      return std::numeric_limits<uint64_t>::max();
    }
    return (1ULL << sz) - 1ULL;
  }

  // multiple objectives optimizations
  void sound_max_all(expr &pre_cond, expr_vector &queries,
                     std::vector<int> &res);

  void sound_min_all(expr &pre_cond, expr_vector &queries,
                     std::vector<int> &res);

  // simple linear search
  bool solve_with_linear_search(expr &pre_cond, expr &query, expr_vector &res,
                                opt_mode = g_min_max);

  // simple binary search
  bool solve_with_binary_search(expr &pre_cond, expr &query, expr_vector &res,
                                opt_mode = g_min_max);

  bool solve_with_compact_search(expr &pre_cond, expr_vector &queries,
                                 std::vector<expr_vector> &res,
                                 opt_mode = g_min_max);

  // reduce to quantified constraint solving
  bool solve_with_qsmt(expr &pre_cond, expr &query, expr_vector &res,
                       opt_mode = g_min_max);

  // use sat encoding (not sure how to)
  // void solve_with_sat(expr& pre_cond, expr& query, expr_vector& res);

  // use z3 internal
  bool solve_with_z3opt(expr &pre_cond, expr &query, expr_vector &res,
                        opt_mode = g_min_max);

  // multi_obj
  bool solve_with_z3opt(expr &pre_cond, expr_vector &query,
                        std::vector<expr_vector> &res, opt_mode = g_min_max);

  void set_verbose_lvl(int lvl) { m_verbose_lvl = lvl; }

private:
  std::vector<tri_bool> compact_check(expr &G, expr_vector &FVec,
                                      expr_vector &queries,
                                      std::vector<int> &bounds);

  bool compact_check_misc(expr &G, expr_vector &FVec,
                          std::vector<tri_bool> &Labels, expr_vector &queries,
                          std::vector<int> &bounds);

public:
  void test_sym_abs();

  void test_sym_abs_multi_obj();

  void solve_file(char const *filename);
};
