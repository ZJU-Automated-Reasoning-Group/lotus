//===-- Verification/Sifa/Domain/IDomain.h --------------------------------===//
//
// Abstract domain interface for Sifa (ported from Ultimate Library-Sifa).
//
// Unlike classical abstract interpretation, Sifa domains may work with
// arbitrary state representations and over-approximate on demand (alpha,
// relation checks). ResultForAlteredInputs carries possibly abstracted inputs.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_IDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_IDOMAIN_H

namespace lotus {
namespace sifa {

/// Result of a relation check (isEqBottom / isSubsetEq) that may have
/// over-approximated the inputs. Ported from Ultimate
/// IDomain.ResultForAlteredInputs.
template <typename StateT> struct ResultForAlteredInputs {
  StateT lhs;
  StateT rhs;
  bool result = false;
  bool abstracted = false;

  ResultForAlteredInputs() = default;
  ResultForAlteredInputs(StateT l, StateT r, bool res = false, bool abs = false)
      : lhs(std::move(l)), rhs(std::move(r)), result(res), abstracted(abs) {}

  const StateT &getLhs() const { return lhs; }
  const StateT &getRhs() const { return rhs; }
  bool isTrueForAbstraction() const { return result; }
  bool wasAbstracted() const { return abstracted; }
};

/// Abstract domain for Symbolic Interpretation with Fluid Abstractions (Sifa).
/// Operators work with any state; abstraction (alpha) is applied when the
/// fluid policy requests it.
template <typename StateT> class IDomain {
public:
  using State = StateT;
  using Result = ResultForAlteredInputs<State>;

  virtual ~IDomain() = default;

  /// Join: over-approximation of union (logical disjunction). (p1 ∨ p2) → j.
  virtual State join(const State &lhs, const State &rhs) const = 0;

  /// Widening: ensures fixpoint in finite iterations on infinite sequences.
  virtual State widen(const State &oldState, const State &widenWith) const = 0;

  /// Check unsatisfiability (equiv. false). May over-approximate input.
  virtual Result isEqBottom(const State &pred) const = 0;

  /// Check subset: subset ⊆ superset. May over-approximate both inputs.
  virtual Result isSubsetEq(const State &subset,
                            const State &superset) const = 0;

  /// Abstraction: over-approximate state (∀ p : p → α(p)). Ideally idempotent.
  virtual State alpha(const State &pred) const = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_IDOMAIN_H
