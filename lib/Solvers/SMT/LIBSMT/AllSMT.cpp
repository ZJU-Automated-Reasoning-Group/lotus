/**
 * @file AllSMT.cpp
 * @brief Implementation of the AllSMTSolver class for enumerating all
 * satisfying models
 *
 * This file implements the AllSMTSolver class, which extends basic SMT solving
 * to enumerate multiple (or all) satisfying models for a given formula. It
 * provides:
 * - Model enumeration up to a specified number
 * - Block-based approach that excludes previously found models
 *
 * The AllSMT approach is useful for applications that need to find all possible
 * satisfying assignments or multiple distinct solutions to an SMT formula.
 */

#include "Solvers/SMT/LIBSMT/AllSMT.h"

#include "z3++.h"
// #include "z3.h"

#include <iostream>

AllSMTSolver::AllSMTSolver() {
  num_vars = 0;
  num_clauses = 0;
}

AllSMTSolver::~AllSMTSolver() {}

int AllSMTSolver::getModels(z3::expr &expr, int k) {
  z3::context &ctx = expr.ctx();
  z3::solver solver(ctx);
  solver.add(expr);
  int found = 0;
  while (solver.check() == z3::sat && k >= 1) {
    found++;
    std::cout << solver << "\n";
    // get model
    z3::model m = solver.get_model();
    // std::cout << m << '\n';
    z3::expr_vector args(ctx);
    for (unsigned i = 0; i < m.size(); i++) {
      // get z3 variable
      z3::func_decl z3Variable = m[i];
      std::string varName = z3Variable.name().str();
      z3::expr exp = m.get_const_interp(z3Variable);
      unsigned bvSize = exp.get_sort().bv_size();
      int value = m.eval(exp).get_numeral_int();
      // std::string svalue = Z3_get_numeral_string(ctx, exp);
      // uniq result
      if (exp.get_sort().is_bv()) {
        // args.push_back(ctx.bv_const(varName.c_str(), bvSize) !=
        // ctx.bv_val(svalue.c_str(), bvSize));
        args.push_back(ctx.bv_const(varName.c_str(), bvSize) !=
                       ctx.bv_val(value, bvSize));
      }
    }
    // block current model
    solver.add(mk_or(args));
    k--;
  }
  return found;
}
