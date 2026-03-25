//===-- Verification/Sifa/Domain/ReachabilityDomain.h ---------------------===//
//
// A minimal concrete domain for end-to-end Sifa wiring: reachability.
//
// State is a boolean; bottom=false means "unreachable", top=true means
// "reachable". join is OR; widen is OR; post is identity.
//
// This is intentionally simple and is primarily used for Sifa reachability
// queries and end-to-end regression coverage.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_REACHABILITYDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_REACHABILITYDOMAIN_H

#include "Verification/Sifa/Domain/AbstractDomain.h"

namespace lotus {
namespace sifa {

template <typename LabelT>
class ReachabilityDomain final : public AbstractDomain<LabelT, bool> {
public:
  using Label = LabelT;
  using State = bool;

  State top() const override { return true; }
  State bottom() const override { return false; }
  bool isBottom(const State &s) const override { return !s; }

  bool leq(const State &a, const State &b) const override { return (!a) || b; }

  State join(const State &a, const State &b) const override { return a || b; }

  State widen(const State &previous, const State &next) const override {
    (void)previous;
    return join(previous, next);
  }

  State meet(const State &a, const State &b) const override { return a && b; }
  bool supportsMeet() const override { return true; }

  State post(const Label &t, const State &in) const override {
    (void)t;
    return in;
  }

  State postCall(const Label &t, const State &callerState) const override {
    (void)t;
    return callerState;
  }
  State postCall(const State &callerState) const override {
    return callerState;
  }

  State postReturn(const Label &t, const State &callerState,
                   const State &calleeSummary) const override {
    (void)t;
    return callerState && calleeSummary;
  }
  State postReturn(const State &callerState,
                   const State &calleeSummary) const override {
    return callerState && calleeSummary;
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_REACHABILITYDOMAIN_H
