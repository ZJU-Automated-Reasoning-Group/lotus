//===-- Verification/Sifa/Fluid/LogSizeWrapperFluid.h ---------------------===//
//
// Fluid wrapper that logs size before delegating (Ultimate-aligned).
//
// Ultimate's LogSizeWrapperFluid logs "Predicate has dag size %s and %d
// disjunct(s). Abstraction will/won't be applied." when debug enabled. Optional
// size/disjunct functions and log callback enable the same.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_FLUID_LOGSIZEWRAPPERFLUID_H
#define LOTUS_VERIFICATION_SIFA_FLUID_LOGSIZEWRAPPERFLUID_H

#include "llvm/ADT/Optional.h"

#include "Verification/Sifa/Fluid/IFluid.h"

#include <cstddef>
#include <functional>

namespace lotus {
namespace sifa {

template <typename StateT>
class LogSizeWrapperFluid final : public IFluid<StateT> {
public:
  using SizeFn = std::function<std::size_t(const StateT &)>;
  using LogFn =
      std::function<void(const StateT &, std::size_t dagSize,
                         std::size_t disjuncts, bool abstractionApplied)>;

  explicit LogSizeWrapperFluid(IFluid<StateT> &inner) : inner_(inner) {}

  /// Ultimate-aligned: optional size/disjunct and log callback for debug
  /// logging.
  LogSizeWrapperFluid(IFluid<StateT> &inner, llvm::Optional<SizeFn> sizeFn,
                      llvm::Optional<SizeFn> disjunctFn,
                      llvm::Optional<LogFn> logFn)
      : inner_(inner), sizeFn_(std::move(sizeFn)),
        disjunctFn_(std::move(disjunctFn)), logFn_(std::move(logFn)) {}

  bool shallBeAbstracted(const StateT &state) const override {
    std::size_t dagSize = sizeFn_ ? (*sizeFn_)(state) : 0;
    std::size_t disjuncts = disjunctFn_ ? (*disjunctFn_)(state) : 0;
    bool result = inner_.shallBeAbstracted(state);
    if (logFn_)
      (*logFn_)(state, dagSize, disjuncts, result);
    return result;
  }

private:
  IFluid<StateT> &inner_;
  llvm::Optional<SizeFn> sizeFn_;
  llvm::Optional<SizeFn> disjunctFn_;
  llvm::Optional<LogFn> logFn_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_FLUID_LOGSIZEWRAPPERFLUID_H
