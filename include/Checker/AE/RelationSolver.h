//===- RelationSolver.h ----Relation Solver for Interval Domains-----------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Checker/AE/AbstractState.h"
#include "Solvers/SMT/LIBSMT/Z3Expr.h"

#include <unordered_map>

namespace lotus {
namespace analysis {

class RelationSolver {
public:
  RelationSolver() = default;

  /* gamma_hat, beta and abstract_consequence works on
  IntervalESBase (the last element of inputs) for RSY or bilateral solver */

  /// Return Z3Expr according to valToValMap
  Z3Expr gamma_hat(const AbstractState &exeState) const;

  /// Return Z3Expr according to another valToValMap
  Z3Expr gamma_hat(const AbstractState &alpha,
                   const AbstractState &exeState) const;

  /// Return Z3Expr from a NodeID
  Z3Expr gamma_hat(uint32_t id, const AbstractState &exeState) const;

  AbstractState abstract_consequence(const AbstractState &lower,
                                     const AbstractState &upper,
                                     const AbstractState &domain) const;

  AbstractState beta(const std::unordered_map<uint32_t, int32_t> &sigma,
                     const AbstractState &exeState) const;

  /// Return Z3 expression lazily based on variable ID
  virtual inline Z3Expr toIntZ3Expr(uint32_t varId) const {
    return Z3Expr::getContext().int_const(std::to_string(varId).c_str());
  }

  inline Z3Expr toIntVal(int32_t f) const {
    return Z3Expr::getContext().int_val(f);
  }

  inline Z3Expr toRealVal(double f) const {
    return Z3Expr::getContext().real_val(std::to_string(f).c_str());
  }

  /* two optional solvers: RSY and bilateral */

  AbstractState bilateral(const AbstractState &domain, const Z3Expr &phi,
                          uint32_t descend_check = 0);

  AbstractState RSY(const AbstractState &domain, const Z3Expr &phi);

  std::unordered_map<uint32_t, int32_t>
  BoxedOptSolver(const Z3Expr &phi, std::unordered_map<uint32_t, int32_t> &ret,
                 std::unordered_map<uint32_t, int32_t> &low_values,
                 std::unordered_map<uint32_t, int32_t> &high_values);

  AbstractState BS(const AbstractState &domain, const Z3Expr &phi);

  void updateMap(std::unordered_map<uint32_t, int32_t> &map, uint32_t key,
                 const int32_t &value);

  void decide_cpa_ext(const Z3Expr &phi, std::unordered_map<uint32_t, Z3Expr> &,
                      std::unordered_map<uint32_t, int32_t> &,
                      std::unordered_map<uint32_t, int32_t> &,
                      std::unordered_map<uint32_t, int32_t> &,
                      std::unordered_map<uint32_t, int32_t> &);
};

} // namespace analysis
} // namespace lotus
