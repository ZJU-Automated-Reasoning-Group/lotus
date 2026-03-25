//===-- Verification/Sifa/Fluid/AlwaysFluid.h -----------------------------===//
//
// Always abstract (ported from Ultimate Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_ALWAYSFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_ALWAYSFLUID_H

#include "Verification/Sifa/Fluid/IFluid.h"

namespace lotus {
namespace sifa {

template <typename StateT> class AlwaysFluid final : public IFluid<StateT> {
public:
  bool shallBeAbstracted(const StateT &state) const override {
    (void)state;
    return true;
  }
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_FLUID_ALWAYSFLUID_H
