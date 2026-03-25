//===-- Verification/Sifa/Domain/StateBasedDomain.h -----------------------===//
//
// Base for domains that use IAbstractState (Ultimate StateBasedDomain-aligned).
//
// Ultimate's StateBasedDomain<STATE> implements IDomain using
// toStates(predicate) and toPredicate(states) via IStateProvider (SMT
// predicates). In lotus we use AbstractDomain<L, State> directly; this header
// documents that state types used in state-based domains should satisfy
// IAbstractState (join, widen, isBottom).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_DOMAIN_STATEBASEDDOMAIN_H
#define LOTUS_VERIFICATION_SIFA_DOMAIN_STATEBASEDDOMAIN_H

#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Domain/IAbstractState.h"

namespace lotus {
namespace sifa {

/// Documented base for domains whose State implements IAbstractState.
/// Ultimate StateBasedDomain uses IStateProvider to convert predicate <->
/// states; lotus uses AbstractDomain<L, State> with State satisfying
/// IAbstractState.
template <typename LabelT, typename StateT>
  requires IAbstractState<StateT>
class StateBasedDomain : public AbstractDomain<LabelT, StateT> {
public:
  using AbstractDomain<LabelT, StateT>::AbstractDomain;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_DOMAIN_STATEBASEDDOMAIN_H
