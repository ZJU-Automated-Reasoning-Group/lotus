/**
 * @file BilateralAnalyzer.cpp
 * @brief Bilateral (bidirectional) algorithm for symbolic abstraction.
    See Algorithm 13 in:
    Thakur, A. V. (2014, August). Symbolic Abstraction: Algorithms and
    Applications (Ph.D. dissertation). Computer Sciences Department, University
    of Wisconsin, Madison.

 * Author: rainoftime
 */
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/ValueMapping.h"
#include "Verification/SymAbsAI/Utils/Utils.h"

namespace symabs_ai {
/**
 * Bi-directional version of strongest consequence using widening and narrowing.
 *
 * Maintains a lower bound and an upper bound on the abstract post-state.
 * In each iteration it computes an abstract consequence `p` between them
 * and then either refines the upper bound (when `p` is unsatisfiable with
 * the concrete semantics) or strengthens the lower bound using a concrete
 * counterexample model. The process stops once the upper bound is below
 * the lower bound in the lattice ordering.
 */
bool BilateralAnalyzer::strongestConsequence(AbstractValue *result,
                                             z3::expr phi,
                                             const ValueMapping &vmap) const {
  using std::unique_ptr;
  bool changed = false;
  z3::solver solver(phi.ctx());
  solver.add(phi);
  unsigned int loop_count = 0;

  auto lower = std::unique_ptr<AbstractValue>(result->clone());

  result->havoc();

  // TODO resource management and timeouts
  while (!((*result) <= (*lower))) {
    vout << "*** lower ***\n" << *lower << "\n";
    vout << "*** upper ***\n" << *result << "\n";
    vout << "loop iteration: " << ++loop_count << "\n";

    auto p = std::unique_ptr<AbstractValue>(lower->clone());
    p->abstractConsequence(*result);

    solver.push();
    solver.add(!p->toFormula(vmap, phi.ctx()));

    auto z3_answer = checkWithStats(&solver);
    assert(z3_answer != z3::unknown);

    if (z3_answer == z3::unsat) {
      vout << "unsat\n"
           << "p {{{\n"
           << *p << "}}}\n";
      result->meetWith(*p);
    } else {
      vout << "sat\n"
           << "model {{{\n"
           << solver.get_model() << "}}}\n";

      auto cstate = ConcreteState(vmap, solver.get_model());
      if (lower->updateWith(cstate))
        changed = true;
    }
    solver.pop();
  }

  // FIXME changed should be checked differently when we work with
  // overapproximations
  return changed;
}

} // namespace symabs_ai
