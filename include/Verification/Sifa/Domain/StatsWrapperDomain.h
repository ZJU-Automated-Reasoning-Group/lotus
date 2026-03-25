//===-- Verification/Sifa/Domain/StatsWrapperDomain.h
//----------------------===//
//
// Domain wrapper that updates SifaStats (Ultimate StatsWrapperDomain-aligned).
//
// Ultimate's StatsWrapperDomain(stats, IDomain) wraps join, widen, isEqBottom,
// isSubsetEq, alpha with start/stop and increment. Lotus: same for
// AbstractDomain operations.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_STATSWRAPPERDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_STATSWRAPPERDOMAIN_H

#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

namespace lotus {
namespace sifa {

template <typename LabelT, typename StateT>
class StatsWrapperDomain final : public AbstractDomain<LabelT, StateT> {
public:
  using Label = LabelT;
  using State = StateT;
  using Domain = AbstractDomain<Label, State>;

  StatsWrapperDomain(SifaStats &stats, const Domain &inner)
      : stats_(stats), inner_(inner) {}

  State top() const override { return inner_.top(); }
  State bottom() const override { return inner_.bottom(); }
  bool isBottom(const State &s) const override { return inner_.isBottom(s); }

  bool leq(const State &a, const State &b) const override {
    stats_.start(SifaStats::Key::DOMAIN_ISSUBSETEQ_TIME);
    stats_.increment(SifaStats::Key::DOMAIN_ISSUBSETEQ_APPLICATIONS);
    bool r = inner_.leq(a, b);
    stats_.stop(SifaStats::Key::DOMAIN_ISSUBSETEQ_TIME);
    return r;
  }
  State join(const State &a, const State &b) const override {
    stats_.start(SifaStats::Key::DOMAIN_JOIN_TIME);
    stats_.increment(SifaStats::Key::DOMAIN_JOIN_APPLICATIONS);
    State r = inner_.join(a, b);
    stats_.stop(SifaStats::Key::DOMAIN_JOIN_TIME);
    return r;
  }
  State widen(const State &prev, const State &next) const override {
    stats_.start(SifaStats::Key::DOMAIN_WIDEN_TIME);
    stats_.increment(SifaStats::Key::DOMAIN_WIDEN_APPLICATIONS);
    State r = inner_.widen(prev, next);
    stats_.stop(SifaStats::Key::DOMAIN_WIDEN_TIME);
    return r;
  }
  State meet(const State &a, const State &b) const override {
    return inner_.meet(a, b);
  }
  bool supportsMeet() const override { return inner_.supportsMeet(); }

  State alpha(const State &s) const override {
    stats_.start(SifaStats::Key::DOMAIN_ALPHA_TIME);
    stats_.increment(SifaStats::Key::DOMAIN_ALPHA_APPLICATIONS);
    State r = inner_.alpha(s);
    stats_.stop(SifaStats::Key::DOMAIN_ALPHA_TIME);
    return r;
  }
  bool isEqBottom(const State &s) const override {
    stats_.start(SifaStats::Key::DOMAIN_ISBOTTOM_TIME);
    stats_.increment(SifaStats::Key::DOMAIN_ISBOTTOM_APPLICATIONS);
    bool r = inner_.isEqBottom(s);
    stats_.stop(SifaStats::Key::DOMAIN_ISBOTTOM_TIME);
    return r;
  }

  State post(const Label &t, const State &in) const override {
    return inner_.post(t, in);
  }
  State postCall(const Label &t, const State &callerState) const override {
    return inner_.postCall(t, callerState);
  }
  State postCall(const State &callerState) const override {
    return inner_.postCall(callerState);
  }
  State postReturn(const Label &t, const State &callerState,
                   const State &calleeSummary) const override {
    return inner_.postReturn(t, callerState, calleeSummary);
  }
  State postReturn(const State &callerState,
                   const State &calleeSummary) const override {
    return inner_.postReturn(callerState, calleeSummary);
  }

private:
  SifaStats &stats_;
  const Domain &inner_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_STATSWRAPPERDOMAIN_H
