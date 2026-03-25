//===-- Verification/Sifa/SymbolicTools.h
//----------------------------------===//
//
// Facade for domain operations (Ultimate SymbolicTools-aligned).
//
// Ultimate's SymbolicTools provides top/bottom predicates, post(trans),
// postCall, postReturn, and(operands), or(operands), isBottomLiteral, etc.,
// backed by SMT (PredicateTransformer, ManagedScript). In lotus we use
// AbstractDomain over LLVM; this class delegates to the domain and
// optionally records SifaStats for TOOLS_POST_* timing.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SYMBOLICTOOLS_H
#define LOTUS_VERIFICATION_SIFA_SYMBOLICTOOLS_H

#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

namespace lotus {
namespace sifa {

/// Facade delegating to AbstractDomain (Ultimate SymbolicTools-aligned).
/// Provides top(), bottom(), post(t,input), postCall(t,input),
/// postReturn(t,caller,summary),
/// isBottomLiteral(state). Optionally records SifaStats for TOOLS_POST_* keys.
template <typename LabelT, typename StateT> class SymbolicTools {
public:
  using Domain = AbstractDomain<LabelT, StateT>;
  using State = StateT;
  using Label = LabelT;

  explicit SymbolicTools(Domain &domain, SifaStats *stats = nullptr)
      : domain_(domain), stats_(stats) {}

  State top() const { return domain_.top(); }
  State bottom() const { return domain_.bottom(); }

  /// Strongest postcondition for transition (Ultimate: post(input,
  /// transition)).
  State post(const Label &t, const State &input) const {
    if (stats_) {
      stats_->start(SifaStats::Key::TOOLS_POST_TIME);
      stats_->increment(SifaStats::Key::TOOLS_POST_APPLICATIONS);
    }
    State r = domain_.post(t, input);
    if (stats_)
      stats_->stop(SifaStats::Key::TOOLS_POST_TIME);
    return r;
  }

  /// State after entering call (Ultimate: postCall(input, callTransition)).
  State postCall(const Label &t, const State &input) const {
    if (stats_) {
      stats_->start(SifaStats::Key::TOOLS_POST_CALL_TIME);
      stats_->increment(SifaStats::Key::TOOLS_POST_CALL_APPLICATIONS);
    }
    State r = domain_.postCall(t, input);
    if (stats_)
      stats_->stop(SifaStats::Key::TOOLS_POST_CALL_TIME);
    return r;
  }

  /// State after return (Ultimate: postReturn(beforeCall, beforeReturn,
  /// returnTrans)).
  State postReturn(const Label &t, const State &callerState,
                   const State &calleeSummary) const {
    if (stats_) {
      stats_->start(SifaStats::Key::TOOLS_POST_RETURN_TIME);
      stats_->increment(SifaStats::Key::TOOLS_POST_RETURN_APPLICATIONS);
    }
    State r = domain_.postReturn(t, callerState, calleeSummary);
    if (stats_)
      stats_->stop(SifaStats::Key::TOOLS_POST_RETURN_TIME);
    return r;
  }

  /// Ultimate: isBottomLiteral(pred) — cheap literal check; delegates to
  /// domain.isBottomLiteral(s).
  bool isBottomLiteral(const State &s) const {
    return domain_.isBottomLiteral(s);
  }

  Domain &getDomain() { return domain_; }
  const Domain &getDomain() const { return domain_; }

  // --- Ultimate API not provided in lotus (SMT-only) ---
  // Ultimate: and(operands), andT(operands), or(operands), orT(operands) —
  // combine predicates/terms. Ultimate: predicate(term), dnfDisjuncts(pred),
  // dnfDisjuncts(pred, termTransformer). Ultimate: isFalse(pred), implies(p1,
  // p2) — solver checks. Lotus uses AbstractDomain over LLVM states; use
  // domain.join/meet and domain.leq for relations.

private:
  Domain &domain_;
  SifaStats *stats_ = nullptr;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SYMBOLICTOOLS_H
