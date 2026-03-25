//===-- Verification/Sifa/Domain/AbstractDomain.h -------------------------===//
//
// Minimal abstract domain interface for Sifa's regex interpreter.
//
// Paper (TACAS 2020 "Ultimate Taipan..."): the post operator computes strongest
// post for star-free regular expressions and optionally applies an abstraction
// (α) when the fluid policy decides. Sifa interprets path-expression regexes:
//  - Literal: post
//  - Union: join
//  - Star: loop summarization (fixpoint + widen)
//
// Ultimate-aligned: IDomain.isEqBottom/isSubsetEq return ResultForAlteredInputs
// so that relation checks may over-approximate and feed back altered states.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_ABSTRACTDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_ABSTRACTDOMAIN_H

#include <utility>

namespace lotus {
namespace sifa {

/// Ultimate-aligned: result of isEqBottom or isSubsetEq when the check may
/// over-approximate inputs. getLhs/getRhs are the (possibly altered) states
/// used for the check; isTrueForAbstraction() is the boolean result.
template <typename StateT> struct ResultForAlteredInputs {
  StateT lhs;
  StateT rhs;
  bool result;
  bool wasAbstracted;

  ResultForAlteredInputs(StateT l, StateT r, bool res, bool abstracted = false)
      : lhs(std::move(l)), rhs(std::move(r)), result(res),
        wasAbstracted(abstracted) {}

  const StateT &getLhs() const { return lhs; }
  const StateT &getRhs() const { return rhs; }
  bool isTrueForAbstraction() const { return result; }
  bool wasAbstractedInputs() const { return wasAbstracted; }
};

template <typename LabelT, typename StateT> class AbstractDomain {
public:
  using Label = LabelT;
  using State = StateT;
  using SubsetEqResult = ResultForAlteredInputs<State>;
  using EqBottomResult =
      ResultForAlteredInputs<State>; // lhs = altered pred, rhs unused

  virtual ~AbstractDomain() = default;

  /// Top element (over-approximation of all states). Ultimate:
  /// SymbolicTools.top() = true.
  virtual State top() const = 0;
  virtual State bottom() const = 0;
  virtual bool isBottom(const State &s) const = 0;

  /// Ultimate-aligned: cheap check for "state is bottom" (e.g. literal
  /// comparison). Default: isBottom(s). Override if domain has a faster literal
  /// check.
  virtual bool isBottomLiteral(const State &s) const { return isBottom(s); }

  /// Over-approximation for fluid abstraction: α(s) ⊇ s. Default: identity.
  virtual State alpha(const State &s) const { return s; }

  /// Equality-to-bottom check (may over-approximate for termination). Default:
  /// isBottom(s).
  virtual bool isEqBottom(const State &s) const { return isBottom(s); }

  /// Ultimate-aligned: isEqBottom returning altered state (for early exit in
  /// DagInterpreter). Default: returns (s, s, isBottom(s), false); override if
  /// domain over-approximates.
  virtual EqBottomResult isEqBottomResult(const State &s) const {
    return EqBottomResult(s, s, isBottom(s), false);
  }

  virtual bool leq(const State &a, const State &b) const = 0;

  /// Equality for cache key lookup (e.g. FixpointLoopSummarizer). Default:
  /// leq(a,b) && leq(b,a).
  virtual bool equal(const State &a, const State &b) const {
    return leq(a, b) && leq(b, a);
  }

  /// Ultimate-aligned: isSubsetEq returning possibly altered lhs/rhs for
  /// fixpoint loop. Default: returns (subset, superset, leq(subset, superset),
  /// false).
  virtual SubsetEqResult subsetEq(const State &subset,
                                  const State &superset) const {
    return SubsetEqResult(subset, superset, leq(subset, superset), false);
  }

  virtual State join(const State &a, const State &b) const = 0;
  virtual State widen(const State &previous, const State &next) const = 0;

  /// Meet (greatest lower bound); used e.g. by ReUseSupersetCallSummarizer.
  /// Default: return \p a (override for domains that support meet).
  virtual State meet(const State &a, const State &b) const {
    (void)b;
    return a;
  }
  /// Whether meet() is a real greatest-lower-bound operation for this domain.
  virtual bool supportsMeet() const { return false; }

  /// Transfer function for a literal transition label.
  virtual State post(const Label &t, const State &in) const = 0;

  /// State after entering a call (caller state projected to callee context).
  /// Used for interprocedural interpretation at ReturnSummary edges.
  virtual State postCall(const Label &t, const State &callerState) const {
    (void)t;
    return postCall(callerState);
  }
  virtual State postCall(const State &callerState) const { return callerState; }

  /// State after return: combine caller state with callee summary.
  /// Default: join(callerState, summary); override for precise return handling.
  virtual State postReturn(const Label &t, const State &callerState,
                           const State &calleeSummary) const {
    (void)t;
    return postReturn(callerState, calleeSummary);
  }
  virtual State postReturn(const State &callerState,
                           const State &calleeSummary) const {
    return join(callerState, calleeSummary);
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_ABSTRACTDOMAIN_H
