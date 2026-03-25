//===-- Verification/Sifa/Fluid/NeverFluid.h ------------------------------===//
//
// Never abstract (ported from Ultimate Sifa).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_NEVERFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_NEVERFLUID_H

#include "Verification/Sifa/Fluid/IFluid.h"

namespace lotus {
namespace sifa {

template <typename StateT> class NeverFluid final : public IFluid<StateT> {
public:
  bool shallBeAbstracted(const StateT &state) const override {
    (void)state;
    return false;
  }
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/SifaSymAbs.h"
extern template class lotus::sifa::NeverFluid<bool>;
extern template class lotus::sifa::NeverFluid<lotus::sifa::SymAbsState>;

#endif // LOTUS_VERIFICATION_SIFA_FLUID_NEVERFLUID_H
