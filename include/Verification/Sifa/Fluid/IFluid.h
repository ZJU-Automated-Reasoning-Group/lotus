//===-- Verification/Sifa/Fluid/IFluid.h ----------------------------------===//
//
// Fluid abstraction policy interface (ported from Ultimate Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan..."): fluids are heuristics that govern
// the choice of abstraction function and when to apply it. They decide when
// to abstract (domain_.alpha) to avoid blow-up; different fluids (NeverFluid,
// SizeLimitFluid, LogSizeWrapperFluid) can be swapped.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_IFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_IFLUID_H

namespace lotus {
namespace sifa {

template <typename StateT> class IFluid {
public:
  virtual ~IFluid() = default;
  virtual bool shallBeAbstracted(const StateT &state) const = 0;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_FLUID_IFLUID_H
