//===-- Verification/Sifa/Domain/RelationCheckUtil.h ----------------------===//
//
// Relation checks isEqBottom / isSubsetEq (Ultimate RelationCheckUtil-aligned).
//
// Ultimate: isEqBottom_SolverAlphaSolver(tools, domain, pred),
// isSubsetEq_SolverAlphaSolver(tools, domain, left, right) using SymbolicTools
// (isFalse, implies) and domain.alpha in a loop. Lotus: no solver; we delegate
// to domain.isBottom and domain.leq; tools overload for API alignment.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_RELATIONCHECKUTIL_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_RELATIONCHECKUTIL_H

#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Domain/IDomain.h"
#include "Verification/Sifa/SymbolicTools.h"

namespace lotus {
namespace sifa {

/// Ultimate RelationCheckUtil: static methods isEqBottom_SolverAlphaSolver,
/// isSubsetEq_SolverAlphaSolver.
template <typename LabelT, typename StateT> class RelationCheckUtil {
public:
  using Domain = AbstractDomain<LabelT, StateT>;
  using State = StateT;
  using Result = ResultForAlteredInputs<State>;
  using Tools = SymbolicTools<LabelT, StateT>;

  /// Ultimate: isEqBottom_SolverAlphaSolver(tools, domain, pred). Lotus: no
  /// solver; one-shot domain.isBottom.
  static Result isEqBottom_SolverAlphaSolver(const Tools &,
                                             const Domain &domain,
                                             const State &s) {
    return isEqBottom_SolverAlphaSolver(domain, s);
  }
  static Result isEqBottom_SolverAlphaSolver(const Domain &domain,
                                             const State &s) {
    Result r(s, domain.bottom(), domain.isBottom(s), false);
    return r;
  }

  /// Ultimate: isSubsetEq_SolverAlphaSolver(tools, domain, left, right). Lotus:
  /// one-shot domain.leq.
  static Result isSubsetEq_SolverAlphaSolver(const Tools &,
                                             const Domain &domain,
                                             const State &subset,
                                             const State &superset) {
    return isSubsetEq_SolverAlphaSolver(domain, subset, superset);
  }
  static Result isSubsetEq_SolverAlphaSolver(const Domain &domain,
                                             const State &subset,
                                             const State &superset) {
    Result r(subset, superset, domain.leq(subset, superset), false);
    return r;
  }

  /// Convenience aliases (same as *_SolverAlphaSolver in lotus).
  static Result isEqBottom(const Domain &domain, const State &s) {
    return isEqBottom_SolverAlphaSolver(domain, s);
  }
  static Result isSubsetEq(const Domain &domain, const State &subset,
                           const State &superset) {
    return isSubsetEq_SolverAlphaSolver(domain, subset, superset);
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_RELATIONCHECKUTIL_H
