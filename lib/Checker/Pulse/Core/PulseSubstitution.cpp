#include "Checker/Pulse/Core/PulseSubstitution.h"

namespace pulse {

void Substitution::add(AbstractValue formal, AbstractValue actual) {
  formal_to_actual_[formal] = actual;
}

llvm::Optional<AbstractValue>
Substitution::substitute(AbstractValue formal) const {
  auto it = formal_to_actual_.find(formal);
  if (it != formal_to_actual_.end()) {
    return it->second;
  }
  return llvm::None;
}

AbstractValue Substitution::substituteOrIdentity(AbstractValue formal) const {
  auto it = formal_to_actual_.find(formal);
  if (it != formal_to_actual_.end()) {
    return it->second;
  }
  return formal;
}

AbstractValue applySubstitution(const Substitution &subst, AbstractValue av) {
  return subst.substituteOrIdentity(av);
}

Address applySubstitution(const Substitution &subst, const Address &addr) {
  AbstractValue substituted_addr = subst.substituteOrIdentity(addr.addr);
  Address result(substituted_addr);
  result.history = addr.history;
  return result;
}

} // namespace pulse
