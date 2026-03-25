//===- RelationSolver.cpp ----Relation Solver for Interval Domains-----------//
//
// Migrated from SVF's AE engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/AE/RelationSolver.h"

#include <climits>
#include <cmath>

namespace lotus {
namespace analysis {

AbstractState RelationSolver::bilateral(const AbstractState &domain,
                                        const Z3Expr &phi,
                                        uint32_t descend_check) {
  /// init variables
  AbstractState upper = domain.top();
  AbstractState lower = domain.bottom();
  uint32_t meets_in_a_row = 0;
  z3::solver solver = Z3Expr::getSolver();
  z3::params p(Z3Expr::getContext());
  /// TODO: add option for timeout
  p.set(":timeout", static_cast<unsigned>(600)); // in milliseconds
  solver.set(p);
  AbstractState consequence;

  /// start processing
  while (lower != upper) {
    if (meets_in_a_row == descend_check) {
      consequence = lower;
    } else {
      consequence = abstract_consequence(lower, upper, domain);
    }
    /// compute domain.model_and(phi,
    /// domain.logic_not(domain.gamma_hat(consequence)))
    Z3Expr rhs = !(gamma_hat(consequence, domain));
    solver.push();
    solver.add(phi.getExpr() && rhs.getExpr());
    std::unordered_map<uint32_t, int32_t> solution;
    z3::check_result checkRes = solver.check();
    /// find any solution, which is sat
    if (checkRes == z3::sat) {
      z3::model m = solver.get_model();
      for (uint32_t i = 0; i < m.size(); i++) {
        z3::func_decl v = m[i];
        if (v.arity() != 0)
          continue;
        solution.emplace(std::stoi(v.name().str()),
                         m.get_const_interp(v).get_numeral_int());
      }
      for (const auto &item : domain.getVarToVal()) {
        if (solution.find(item.first) == solution.end()) {
          solution.emplace(item.first, 0);
        }
      }
      solver.pop();
      AbstractState newLower = domain.bottom();
      newLower.joinWith(lower);
      AbstractState rhs = beta(solution, domain);
      newLower.joinWith(rhs);
      lower = newLower;
      meets_in_a_row = 0;
    } else /// unknown or unsat
    {
      solver.pop();
      if (checkRes == z3::unknown) {
        /// for timeout reason return upper
        if (solver.reason_unknown() == "timeout")
          return upper;
      }
      AbstractState newUpper = domain.top();
      newUpper.meetWith(upper);
      newUpper.meetWith(consequence);
      upper = newUpper;
      meets_in_a_row += 1;
    }
  }
  return upper;
}

AbstractState RelationSolver::RSY(const AbstractState &domain,
                                  const Z3Expr &phi) {
  AbstractState lower = domain.bottom();
  z3::solver &solver = Z3Expr::getSolver();
  z3::params p(Z3Expr::getContext());
  /// TODO: add option for timeout
  p.set(":timeout", static_cast<unsigned>(600)); // in milliseconds
  solver.set(p);
  while (1) {
    Z3Expr rhs = !(gamma_hat(lower, domain));
    solver.push();
    solver.add(phi.getExpr() && rhs.getExpr());
    std::unordered_map<uint32_t, int32_t> solution;
    z3::check_result checkRes = solver.check();
    /// find any solution, which is sat
    if (checkRes == z3::sat) {
      z3::model m = solver.get_model();
      for (uint32_t i = 0; i < m.size(); i++) {
        z3::func_decl v = m[i];
        if (v.arity() != 0)
          continue;

        solution.emplace(std::stoi(v.name().str()),
                         m.get_const_interp(v).get_numeral_int());
      }
      for (const auto &item : domain.getVarToVal()) {
        if (solution.find(item.first) == solution.end()) {
          solution.emplace(item.first, 0);
        }
      }
      solver.pop();
      AbstractState newLower = domain.bottom();
      newLower.joinWith(lower);
      newLower.joinWith(beta(solution, domain));
      lower = newLower;
    } else /// unknown or unsat
    {
      solver.pop();
      if (checkRes == z3::unknown) {
        /// for timeout reason return upper
        if (solver.reason_unknown() == "timeout")
          return domain.top();
      }
      break;
    }
  }
  return lower;
}

AbstractState
RelationSolver::abstract_consequence(const AbstractState &lower,
                                     const AbstractState &upper,
                                     const AbstractState &domain) const {
  /*Returns the "abstract consequence" of lower and upper.

  The abstract consequence must be a superset of lower and *NOT* a
  superset of upper.

          Note that this is a fairly "simple" abstract consequence, in that it
  sets only one variable to a non-top interval. This improves performance
  of the SMT solver in many cases. In certain cases, other choices for
  the abstract consequence will lead to better algorithm performance.*/

  for (auto it = domain.getVarToVal().begin(); it != domain.getVarToVal().end();
       ++it)
  /// for variable in self.variables:
  {
    AbstractState proposed = domain.top(); /// proposed = self.top.copy()
    proposed[it->first] = AbstractValue(lower[it->first].getInterval());
    /// proposed.set_interval(variable, lower.interval_of(variable))
    /// proposed._locToItvVal
    if (!(proposed >= upper)) /// if not proposed >= upper:
    {
      return proposed; /// return proposed
    }
  }
  return lower; /// return lower.copy()
}

Z3Expr RelationSolver::gamma_hat(const AbstractState &exeState) const {
  Z3Expr res(Z3Expr::getContext().bool_val(true));
  for (auto &item : exeState.getVarToVal()) {
    IntervalValue interval = item.second.getInterval();
    if (interval.isBottom())
      return Z3Expr::getContext().bool_val(false);
    if (interval.isTop())
      continue;
    Z3Expr v = toIntZ3Expr(item.first);
    res = (res && v >= (int)interval.lb().getIntNumeral() &&
           v <= (int)interval.ub().getIntNumeral())
              .simplify();
  }
  return res;
}

Z3Expr RelationSolver::gamma_hat(const AbstractState &alpha,
                                 const AbstractState &exeState) const {
  Z3Expr res(Z3Expr::getContext().bool_val(true));
  for (auto &item : exeState.getVarToVal()) {
    IntervalValue interval = alpha[item.first].getInterval();
    if (interval.isBottom())
      return Z3Expr::getContext().bool_val(false);
    if (interval.isTop())
      continue;
    Z3Expr v = toIntZ3Expr(item.first);
    res = (res && v >= (int)interval.lb().getIntNumeral() &&
           v <= (int)interval.ub().getIntNumeral())
              .simplify();
  }
  return res;
}

Z3Expr RelationSolver::gamma_hat(uint32_t id,
                                 const AbstractState &exeState) const {
  auto it = exeState.getVarToVal().find(id);
  assert(it != exeState.getVarToVal().end() && "id not in varToVal?");
  Z3Expr v = toIntZ3Expr(id);
  Z3Expr res = (v >= (int)it->second.getInterval().lb().getIntNumeral() &&
                v <= (int)it->second.getInterval().ub().getIntNumeral());
  return res;
}

AbstractState
RelationSolver::beta(const std::unordered_map<uint32_t, int32_t> &sigma,
                     const AbstractState &exeState) const {
  AbstractState res;
  for (const auto &item : exeState.getVarToVal()) {
    int32_t val = sigma.at(item.first);
    res[item.first] = AbstractValue(
        IntervalValue(static_cast<int64_t>(val), static_cast<int64_t>(val)));
  }
  return res;
}

void RelationSolver::updateMap(std::unordered_map<uint32_t, int32_t> &map,
                               uint32_t key, const int32_t &value) {
  auto it = map.find(key);
  if (it == map.end()) {
    map.emplace(key, value);
  } else {
    it->second = value;
  }
}

AbstractState RelationSolver::BS(const AbstractState &domain,
                                 const Z3Expr &phi) {
  /// because key of _varToItvVal is u32_t, -key may out of range for int
  /// so we do key + bias for -key
  uint32_t bias = 0;
  int32_t infinity = INT32_MAX / 2 - 1;

  std::unordered_map<uint32_t, int32_t> ret;
  std::unordered_map<uint32_t, int32_t> low_values, high_values;
  Z3Expr new_phi = phi;
  /// init low, ret, high
  for (const auto &item : domain.getVarToVal()) {
    IntervalValue interval = item.second.getInterval();
    updateMap(ret, item.first,
              static_cast<int32_t>(interval.ub().getIntNumeral()));
    if (interval.lb().is_minus_infinity())
      updateMap(low_values, item.first, -infinity);
    else
      updateMap(low_values, item.first,
                static_cast<int32_t>(interval.lb().getIntNumeral()));
    if (interval.ub().is_plus_infinity())
      updateMap(high_values, item.first, infinity);
    else
      updateMap(high_values, item.first,
                static_cast<int32_t>(interval.ub().getIntNumeral()));
    if (item.first > bias)
      bias = item.first + 1;
  }
  for (const auto &item : domain.getVarToVal()) {
    /// init objects -x
    IntervalValue interval = item.second.getInterval();
    uint32_t reverse_key = item.first + bias;
    updateMap(ret, reverse_key,
              static_cast<int32_t>(-interval.lb().getIntNumeral()));
    if (interval.ub().is_plus_infinity())
      updateMap(low_values, reverse_key, -infinity);
    else
      updateMap(low_values, reverse_key,
                static_cast<int32_t>(-interval.ub().getIntNumeral()));
    if (interval.lb().is_minus_infinity())
      updateMap(high_values, reverse_key, infinity);
    else
      updateMap(high_values, reverse_key,
                static_cast<int32_t>(-interval.lb().getIntNumeral()));
    /// add a relation that x == -(x+bias)
    new_phi =
        (new_phi && (toIntZ3Expr(reverse_key) == -1 * toIntZ3Expr(item.first)));
  }
  /// optimize each object
  BoxedOptSolver(new_phi.simplify(), ret, low_values, high_values);
  /// fill in the return values
  AbstractState retInv;
  for (const auto &item : ret) {
    if (item.first >= bias) {
      if (!retInv.inVarToValTable(item.first - bias))
        retInv[item.first - bias] = AbstractValue(IntervalValue::top());

      if (item.second == (infinity))
        retInv[item.first - bias] = AbstractValue(
            IntervalValue(BoundedInt::minus_infinity(),
                          retInv[item.first - bias].getInterval().ub()));
      else
        retInv[item.first - bias] = AbstractValue(
            IntervalValue(BoundedInt(static_cast<int64_t>(-item.second)),
                          retInv[item.first - bias].getInterval().ub()));
    } else {
      if (item.second == (infinity))
        retInv[item.first] =
            AbstractValue(IntervalValue(retInv[item.first].getInterval().lb(),
                                        BoundedInt::plus_infinity()));
      else
        retInv[item.first] = AbstractValue(
            IntervalValue(retInv[item.first].getInterval().lb(),
                          BoundedInt(static_cast<int64_t>(item.second))));
    }
  }
  return retInv;
}

std::unordered_map<uint32_t, int32_t> RelationSolver::BoxedOptSolver(
    const Z3Expr &phi, std::unordered_map<uint32_t, int32_t> &ret,
    std::unordered_map<uint32_t, int32_t> &low_values,
    std::unordered_map<uint32_t, int32_t> &high_values) {
  /// this is the S in the original paper
  std::unordered_map<uint32_t, Z3Expr> L_phi;
  std::unordered_map<uint32_t, int32_t> mid_values;
  while (1) {
    L_phi.clear();
    for (const auto &item : ret) {
      Z3Expr v = toIntZ3Expr(item.first);
      if (low_values.at(item.first) <= (high_values.at(item.first))) {
        int32_t mid =
            (low_values.at(item.first) +
             (high_values.at(item.first) - low_values.at(item.first)) / 2);
        updateMap(mid_values, item.first, mid);
        Z3Expr expr =
            (toIntVal(mid) <= v && v <= toIntVal(high_values.at(item.first)));
        L_phi[item.first] = expr;
      }
    }
    if (L_phi.empty())
      break;
    else
      decide_cpa_ext(phi, L_phi, mid_values, ret, low_values, high_values);
  }
  return ret;
}

void RelationSolver::decide_cpa_ext(
    const Z3Expr &phi, std::unordered_map<uint32_t, Z3Expr> &L_phi,
    std::unordered_map<uint32_t, int32_t> &mid_values,
    std::unordered_map<uint32_t, int32_t> &ret,
    std::unordered_map<uint32_t, int32_t> &low_values,
    std::unordered_map<uint32_t, int32_t> &high_values) {
  while (1) {
    Z3Expr join_expr(Z3Expr::getContext().bool_val(false));
    for (const auto &item : L_phi)
      join_expr = (join_expr || item.second);
    join_expr = (join_expr && phi).simplify();
    z3::solver &solver = Z3Expr::getSolver();
    solver.push();
    solver.add(join_expr.getExpr());
    std::unordered_map<uint32_t, double> solution;
    z3::check_result checkRes = solver.check();
    /// find any solution, which is sat
    if (checkRes == z3::sat) {
      z3::model m = solver.get_model();
      solver.pop();
      for (const auto &item : L_phi) {
        uint32_t id = item.first;
        int value = m.eval(toIntZ3Expr(id).getExpr()).get_numeral_int();
        /// id is the var id, value is the solution found for var_id
        /// add a relation to check if the solution meets phi_id
        Z3Expr expr = (item.second && toIntZ3Expr(id) == value);
        solver.push();
        solver.add(expr.getExpr());
        // solution meets phi_id
        if (solver.check() == z3::sat) {
          updateMap(ret, id, (value));
          updateMap(low_values, id, ret.at(id) + 1);

          int32_t mid = (low_values.at(id) + high_values.at(id) + 1) / 2;
          updateMap(mid_values, id, mid);
          Z3Expr v = toIntZ3Expr(id);
          Z3Expr expr = (toIntVal(mid_values.at(id)) <= v &&
                         v <= toIntVal(high_values.at(id)));
          L_phi[id] = expr;
        }
        solver.pop();
      }
    } else /// unknown or unsat, we consider unknown as unsat
    {
      solver.pop();
      for (const auto &item : L_phi)
        high_values.at(item.first) = mid_values.at(item.first) - 1;
      return;
    }
  }
}

} // namespace analysis
} // namespace lotus
