//===-- Verification/Sifa/Fluid/SizeLimitFluid.h --------------------------===//
//
// Abstract when a state exceeds size/disjunct limits (ported from Ultimate
// Sifa).
//
// Ultimate: maxDagSize + DAGSize.size(term), maxDisjuncts +
// numberOfDisjuncts(term). Negative limit disables that check; caller provides
// sizeFn and optionally disjunctFn.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_SIZELIMITFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_SIZELIMITFLUID_H

#include "Verification/Sifa/Fluid/IFluid.h"

#include <cstddef>
#include <functional>
#include <optional>

namespace lotus {
namespace sifa {

template <typename StateT> class SizeLimitFluid final : public IFluid<StateT> {
public:
  using SizeFn = std::function<std::size_t(const StateT &)>;

  /// Single limit (dag size). Use sizeFn(state) > limit; negative limit
  /// disables.
  SizeLimitFluid(std::ptrdiff_t maxDagSize, SizeFn sizeFn)
      : maxDagSize_(maxDagSize), sizeFn_(std::move(sizeFn)) {}

  /// Ultimate-aligned: both maxDagSize and maxDisjuncts. Negative value
  /// disables.
  SizeLimitFluid(std::ptrdiff_t maxDagSize, SizeFn sizeFn,
                 std::ptrdiff_t maxDisjuncts,
                 std::function<std::size_t(const StateT &)> disjunctFn)
      : maxDagSize_(maxDagSize), sizeFn_(std::move(sizeFn)),
        maxDisjuncts_(maxDisjuncts), disjunctFn_(std::move(disjunctFn)) {}

  bool shallBeAbstracted(const StateT &state) const override {
    if (maxDagSize_ >= 0 && sizeFn_ &&
        sizeFn_(state) > static_cast<std::size_t>(maxDagSize_))
      return true;
    if (maxDisjuncts_ >= 0 && disjunctFn_ &&
        disjunctFn_(state) > static_cast<std::size_t>(maxDisjuncts_))
      return true;
    return false;
  }

private:
  std::ptrdiff_t maxDagSize_ = -1;
  SizeFn sizeFn_;
  std::ptrdiff_t maxDisjuncts_ = -1;
  std::function<std::size_t(const StateT &)> disjunctFn_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_FLUID_SIZELIMITFLUID_H
