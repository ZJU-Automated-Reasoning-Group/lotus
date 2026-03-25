//===-- Verification/Sifa/Fluid/StatsWrapperFluid.h
//------------------------===//
//
// Fluid wrapper that records stats before delegating (Ultimate-aligned).
//
// Ultimate's StatsWrapperFluid increments statistics when shallBeAbstracted
// is called and/or when abstraction is applied.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_STATSWRAPPERFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_STATSWRAPPERFLUID_H

#include "Verification/Sifa/Fluid/IFluid.h"
#include "Verification/Sifa/Statistics/SifaStats.h"

namespace lotus {
namespace sifa {

template <typename StateT>
class StatsWrapperFluid final : public IFluid<StateT> {
public:
  StatsWrapperFluid(SifaStats &stats, IFluid<StateT> &inner)
      : stats_(stats), inner_(inner) {}

  bool shallBeAbstracted(const StateT &state) const override {
    stats_.increment(SifaStats::Key::FLUID_QUERIES);
    stats_.start(SifaStats::Key::FLUID_QUERY_TIME);
    bool result = inner_.shallBeAbstracted(state);
    stats_.stop(SifaStats::Key::FLUID_QUERY_TIME);
    if (result)
      stats_.increment(SifaStats::Key::FLUID_YES_ANSWERS);
    return result;
  }

private:
  SifaStats &stats_;
  IFluid<StateT> &inner_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_FLUID_STATSWRAPPERFLUID_H
