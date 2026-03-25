#ifndef CHECKER_PULSE_PULSELATENTISSUE_H
#define CHECKER_PULSE_PULSELATENTISSUE_H

#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Domain/PulseExecutionDomain.h"

#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace llvm {
class Instruction;
class Function;
} // namespace llvm

namespace pulse {

class PulseSummary;

/**
 * LatentIssue: represents a potential issue that should be delayed
 * until it becomes "manifest" (can occur in any reasonable calling context)
 */
class LatentIssue {
public:
  enum class IssueKind {
    InvalidAccess,    // Access to invalid address
    OutOfBounds,      // Out-of-bounds access
    NullDereference,  // Null pointer dereference
    UseAfterFree,     // Use after free
    UninitializedRead // Read from uninitialized memory
  };

private:
  OperationResult diagnostic_;
  IssueKind kind_;
  AbstractValue address_;
  const llvm::Instruction *location_;
  Trace trace_;
  std::vector<std::pair<const llvm::Function *, const llvm::Instruction *>>
      calling_context_;

public:
  LatentIssue(OperationResult diagnostic, IssueKind k, AbstractValue addr,
              const llvm::Instruction *loc, Trace trace)
      : diagnostic_(diagnostic), kind_(k), address_(addr), location_(loc),
        trace_(std::move(trace)) {}

  OperationResult getDiagnostic() const { return diagnostic_; }
  IssueKind getKind() const { return kind_; }
  AbstractValue getAddress() const { return address_; }
  const llvm::Instruction *getLocation() const { return location_; }
  const Trace &getTrace() const { return trace_; }
  const std::vector<
      std::pair<const llvm::Function *, const llvm::Instruction *>> &
  getCallingContext() const {
    return calling_context_;
  }

  void addCallingContext(const llvm::Function *func,
                         const llvm::Instruction *call_site) {
    calling_context_.emplace_back(func, call_site);
  }

  /**
   * Check if this issue should be reported now or delayed
   * Returns true if issue is manifest (should be reported)
   */
  static bool shouldReport(const PulseSummary &summary,
                           const LatentIssue &issue);

  /** Map OperationResult to IssueKind for latent issue recording. */
  static IssueKind issueKindFromResult(OperationResult result);

  /**
   * Check if a state is manifest (can occur in any reasonable calling context)
   * A state is manifest if its path condition is empty or only contains
   * facts about allocated pointers being non-null
   */
  static bool isManifest(const AbductiveDomain &astate);

  /**
   * Operation-specific manifest check.
   * Used at reporting sites to avoid over-approximating manifestness from the
   * whole state only.
   */
  static bool isManifest(OperationResult diagnostic,
                         const AbductiveDomain &astate, AbstractValue address);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSELATENTISSUE_H
