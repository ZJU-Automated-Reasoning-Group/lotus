//===-- Verification/Sifa/Domain/IAbstractState.h
//--------------------------===//
//
// Interface for abstract states used in StateBasedDomain (Ultimate-aligned).
//
// Ultimate's StateBasedDomain<STATE> requires STATE to implement
// IAbstractState: join(other), widen(other), isBottom(). Lotus uses the same
// contract for state types that are merged (e.g. NonrelationalState,
// IntervalState).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_IABSTRACTSTATE_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_IABSTRACTSTATE_H

#include <concepts>

namespace lotus {
namespace sifa {

/// Concept for abstract state used in StateBasedDomain (Ultimate
/// IAbstractState). Requirements: join(other), widen(other), isBottom().
template <typename State>
concept IAbstractState = requires(const State &a, const State &b) {
  { a.join(b) } -> std::same_as<State>;
  { a.widen(b) } -> std::same_as<State>;
  { a.isBottom() } -> std::same_as<bool>;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_IABSTRACTSTATE_H
