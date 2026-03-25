#include "Checker/Pulse/Report/PulseLatentIssue.h"

#include "Checker/Pulse/Interproc/PulseSummary.h"

#include <llvm/IR/Argument.h>

namespace pulse {

bool LatentIssue::shouldReport(const PulseSummary & /*summary*/,
                               const LatentIssue & /*issue*/) {
  // Used when we have a stored latent issue and summary; typically we decide
  // at detection site via isManifest(astate). This remains for API
  // compatibility.
  return true;
}

LatentIssue::IssueKind
LatentIssue::issueKindFromResult(OperationResult result) {
  switch (result) {
  case OperationResult::InvalidAccess:
    return IssueKind::InvalidAccess;
  case OperationResult::OutOfBounds:
    return IssueKind::OutOfBounds;
  case OperationResult::UseAfterFree:
    return IssueKind::UseAfterFree;
  case OperationResult::NullDereference:
    return IssueKind::NullDereference;
  case OperationResult::UninitializedRead:
    return IssueKind::UninitializedRead;
  case OperationResult::TaintError:
  case OperationResult::Success:
    return IssueKind::InvalidAccess;
  }
  return IssueKind::InvalidAccess;
}

bool LatentIssue::isManifest(const AbductiveDomain &astate) {
  // A state is manifest if it doesn't rely on restrictive assumptions
  // (e.g., ptr == null) and we haven't introduced unknown values.
  return !astate.hasUnknownValues() &&
         astate.getPathFormula().isEmptyOrTrivial();
}

bool LatentIssue::isManifest(OperationResult diagnostic,
                             const AbductiveDomain &astate,
                             AbstractValue address) {
  // Null dereferences are only reported when we can trace the nullness to a
  // null pointer constant (see PulseOperations::isNullConstantSource), so they
  // are manifest by construction.
  if (diagnostic == OperationResult::NullDereference) {
    return true;
  }

  // Use-after-free is manifest once we have observed the invalidation along
  // the current path.
  if (diagnostic == OperationResult::UseAfterFree) {
    return true;
  }

  if (diagnostic == OperationResult::OutOfBounds) {
    return true;
  }

  // Uninitialized reads from parameters are highly dependent on caller
  // context; delay them as latent. Local uninitialized reads are manifest.
  if (diagnostic == OperationResult::UninitializedRead) {
    const llvm::Value *v = address.getValue();
    return v ? !llvm::isa<llvm::Argument>(v) : false;
  }

  // Default: use the state-level heuristic.
  return isManifest(astate);
}

} // namespace pulse
