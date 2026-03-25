#include "Checker/Pulse/Core/PulseCallState.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"

namespace pulse {

bool CallState::incorporateNewEqs(const PulseFormula &new_eqs) {
  // Incorporate new equalities into astate
  // This is a simplified version - full implementation would:
  // 1. Add equalities to astate's path formula
  // 2. Normalize values in subst and rev_subst
  // 3. Update hist_map if needed

  // Merge new_eqs into astate's path formula
  if (astate_) {
    PulseFormula merged =
        PulseFormula::merge(astate_->getPathFormula(), new_eqs);
    if (!merged.isConsistent()) {
      return false; // Contradiction detected
    }
    astate_->setPathFormula(std::make_unique<PulseFormula>(std::move(merged)));
  }

  // Normalize substitution values
  // This would require iterating through subst and normalizing each value
  // For now, we'll just return true (success)

  return true;
}

} // namespace pulse
