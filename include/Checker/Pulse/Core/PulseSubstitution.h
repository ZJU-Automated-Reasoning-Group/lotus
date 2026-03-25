#ifndef CHECKER_PULSE_PULSESUBSTITUTION_H
#define CHECKER_PULSE_PULSESUBSTITUTION_H

#include "Checker/Pulse/Domain/PulseDomain.h"

#include <map>

namespace pulse {

/**
 * Substitution: maps formal abstract values (from callee) to actual abstract
 * values (in caller)
 */
class Substitution {
private:
  std::map<AbstractValue, AbstractValue> formal_to_actual_;

public:
  void add(AbstractValue formal, AbstractValue actual);
  llvm::Optional<AbstractValue> substitute(AbstractValue formal) const;
  AbstractValue substituteOrIdentity(AbstractValue formal) const;

  bool empty() const { return formal_to_actual_.empty(); }
  void clear() { formal_to_actual_.clear(); }
};

/**
 * Apply substitution to an abstract value
 */
AbstractValue applySubstitution(const Substitution &subst, AbstractValue av);

/**
 * Apply substitution to an address
 */
Address applySubstitution(const Substitution &subst, const Address &addr);

} // namespace pulse

#endif // CHECKER_PULSE_PULSESUBSTITUTION_H
