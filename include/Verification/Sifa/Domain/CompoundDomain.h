//===-- Verification/Sifa/Domain/CompoundDomain.h -------------------------===//
//
// Product of multiple domains (Ultimate CompoundDomain-aligned).
//
// Ultimate's CompoundDomain(SymbolicTools, Collection<IDomain>) delegates
// join/widen/isEqBottom/isSubsetEq/alpha to each subdomain and combines
// results with and(...). In lotus we use a product state: join and widen
// are component-wise; bottom is (bottom_1,...,bottom_n).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_COMPOUNDDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_COMPOUNDDOMAIN_H

#include "Verification/Sifa/Domain/AbstractDomain.h"

#include <tuple>
#include <utility>

namespace lotus {
namespace sifa {

/// Product of two domains (Ultimate CompoundDomain for N=2).
/// State = (State1, State2); all operations component-wise.
template <typename LabelT, typename State1, typename State2>
class CompoundDomain final
    : public AbstractDomain<LabelT, std::pair<State1, State2>> {
public:
  using Label = LabelT;
  using State = std::pair<State1, State2>;
  using Domain1 = AbstractDomain<Label, State1>;
  using Domain2 = AbstractDomain<Label, State2>;

  CompoundDomain(const Domain1 &d1, const Domain2 &d2) : d1_(d1), d2_(d2) {}

  State top() const override { return {d1_.top(), d2_.top()}; }
  State bottom() const override { return {d1_.bottom(), d2_.bottom()}; }
  bool isBottom(const State &s) const override {
    return d1_.isBottom(s.first) || d2_.isBottom(s.second);
  }

  bool leq(const State &a, const State &b) const override {
    return d1_.leq(a.first, b.first) && d2_.leq(a.second, b.second);
  }
  State join(const State &a, const State &b) const override {
    return {d1_.join(a.first, b.first), d2_.join(a.second, b.second)};
  }
  State widen(const State &prev, const State &next) const override {
    return {d1_.widen(prev.first, next.first),
            d2_.widen(prev.second, next.second)};
  }
  State meet(const State &a, const State &b) const override {
    return {d1_.meet(a.first, b.first), d2_.meet(a.second, b.second)};
  }
  bool supportsMeet() const override {
    return d1_.supportsMeet() && d2_.supportsMeet();
  }

  State alpha(const State &s) const override {
    return {d1_.alpha(s.first), d2_.alpha(s.second)};
  }
  bool isEqBottom(const State &s) const override {
    return d1_.isEqBottom(s.first) || d2_.isEqBottom(s.second);
  }

  State post(const Label &t, const State &in) const override {
    return {d1_.post(t, in.first), d2_.post(t, in.second)};
  }
  State postCall(const Label &t, const State &callerState) const override {
    return {d1_.postCall(t, callerState.first),
            d2_.postCall(t, callerState.second)};
  }
  State postCall(const State &callerState) const override {
    return {d1_.postCall(callerState.first), d2_.postCall(callerState.second)};
  }
  State postReturn(const Label &t, const State &callerState,
                   const State &calleeSummary) const override {
    return {d1_.postReturn(t, callerState.first, calleeSummary.first),
            d2_.postReturn(t, callerState.second, calleeSummary.second)};
  }
  State postReturn(const State &callerState,
                   const State &calleeSummary) const override {
    return {d1_.postReturn(callerState.first, calleeSummary.first),
            d2_.postReturn(callerState.second, calleeSummary.second)};
  }

private:
  const Domain1 &d1_;
  const Domain2 &d2_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_COMPOUNDDOMAIN_H
