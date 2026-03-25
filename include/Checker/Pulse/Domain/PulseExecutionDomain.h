#ifndef CHECKER_PULSE_PULSEEXECUTIONDOMAIN_H
#define CHECKER_PULSE_PULSEEXECUTIONDOMAIN_H

#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"

#include <string>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace pulse {

// Forward declaration to break circular dependency
class LatentIssue;

/**
 * OperationResult: result of an operation (success or error type)
 */
enum class OperationResult {
  Success,
  InvalidAccess,     // Access to invalid memory
  OutOfBounds,       // Proven out-of-bounds memory access
  NullDereference,   // Null pointer dereference
  UseAfterFree,      // Use after free
  UninitializedRead, // Read from uninitialized memory
  TaintError         // Taint error detected
};

// Legacy enum for backward compatibility
enum class ExecutionState {
  ContinueProgram = 0,
  ExceptionRaised = 1,
  Stopped = 2
};

/**
 * StoppedExecution: represents different reasons why execution stopped
 */
enum class StoppedExecutionKind {
  ExitProgram,               // Normal exit (function returns)
  AbortProgram,              // Error detected and reported
  LatentAbortProgram,        // Potential error (latent issue)
  LatentInvalidAccess,       // Latent invalid access with calling context
  LatentSpecializedTypeIssue // Latent type specialization issue
};

/**
 * ExecutionDomain: execution states with proper variants
 * Aligned with Infer's ExecutionDomain structure
 */
class ExecutionDomain {
public:
  // Variant types for stopped execution
  struct StoppedExecution {
    StoppedExecutionKind kind;
    std::unique_ptr<AbductiveDomain> astate; // Full state
    llvm::Optional<AbstractValue> return_value;

    // For AbortProgram
    OperationResult diagnostic;
    Trace trace_to_issue;

    // For LatentAbortProgram - use raw pointer to avoid incomplete type issues
    LatentIssue *latent_issue; // Owned elsewhere, or nullptr

    // For LatentInvalidAccess
    AbstractValue address;
    std::pair<Trace, llvm::Optional<InvalidationKind>> must_be_valid;
    std::vector<std::pair<const llvm::Function *, const llvm::Instruction *>>
        calling_context;

    // For LatentSpecializedTypeIssue
    std::string specialized_type;
    Trace type_trace;
    StoppedExecution()
        : kind(StoppedExecutionKind::ExitProgram),
          diagnostic(OperationResult::Success), latent_issue(nullptr) {}

    // Copy constructor
    StoppedExecution(const StoppedExecution &other)
        : kind(other.kind),
          astate(other.astate
                     ? std::make_unique<AbductiveDomain>(other.astate->clone())
                     : nullptr),
          return_value(other.return_value),
          diagnostic(other.diagnostic),
          trace_to_issue(other.trace_to_issue.clone()),
          latent_issue(other.latent_issue), // Pointer, not owned
          address(other.address),
          must_be_valid(std::make_pair(other.must_be_valid.first.clone(),
                                       other.must_be_valid.second)),
          calling_context(other.calling_context),
          specialized_type(other.specialized_type),
          type_trace(other.type_trace.clone()) {}

    // Copy assignment operator
    StoppedExecution &operator=(const StoppedExecution &other) {
      if (this != &other) {
        kind = other.kind;
        astate = other.astate
                     ? std::make_unique<AbductiveDomain>(other.astate->clone())
                     : nullptr;
        return_value = other.return_value;
        diagnostic = other.diagnostic;
        trace_to_issue = other.trace_to_issue.clone();
        latent_issue = other.latent_issue; // Pointer, not owned
        address = other.address;
        must_be_valid = std::make_pair(other.must_be_valid.first.clone(),
                                       other.must_be_valid.second);
        calling_context = other.calling_context;
        specialized_type = other.specialized_type;
        type_trace = other.type_trace.clone();
      }
      return *this;
    }

    // Move constructor
    StoppedExecution(StoppedExecution &&) noexcept = default;

    // Move assignment operator
    StoppedExecution &operator=(StoppedExecution &&) noexcept = default;

    ~StoppedExecution() {
      // LatentIssue is managed elsewhere, don't delete here
    }

    // Reset to default state
    void reset() {
      kind = StoppedExecutionKind::ExitProgram;
      astate.reset();
      return_value = llvm::None;
      latent_issue = nullptr;
      diagnostic = OperationResult::Success;
      trace_to_issue = Trace();
      address = AbstractValue();
      must_be_valid = std::make_pair(Trace(), llvm::None);
      calling_context.clear();
      specialized_type.clear();
      type_trace = Trace();
    }
  };

private:
  enum class Variant { ContinueProgram, ExceptionRaised, Stopped };

  Variant variant_;
  std::unique_ptr<AbductiveDomain> astate_;
  StoppedExecution stopped_execution_;
  const llvm::BasicBlock *entry_pred_{nullptr};

public:
  ExecutionDomain()
      : variant_(Variant::ContinueProgram),
        astate_(std::make_unique<AbductiveDomain>()) {}

  explicit ExecutionDomain(std::unique_ptr<AbductiveDomain> astate)
      : variant_(Variant::ContinueProgram), astate_(std::move(astate)) {}

  ~ExecutionDomain() = default;

  ExecutionDomain(const ExecutionDomain &other)
      : variant_(other.variant_),
        astate_(other.astate_
                    ? std::make_unique<AbductiveDomain>(other.astate_->clone())
                    : nullptr),
        entry_pred_(other.entry_pred_) {
    // Deep copy stopped_execution_ if needed
    if (other.variant_ == Variant::Stopped) {
      stopped_execution_.kind = other.stopped_execution_.kind;
      if (other.stopped_execution_.astate) {
        stopped_execution_.astate = std::make_unique<AbductiveDomain>(
            other.stopped_execution_.astate->clone());
      }
      stopped_execution_.return_value = other.stopped_execution_.return_value;
      stopped_execution_.diagnostic = other.stopped_execution_.diagnostic;
      stopped_execution_.trace_to_issue =
          other.stopped_execution_.trace_to_issue.clone();
      stopped_execution_.latent_issue =
          other.stopped_execution_.latent_issue; // Pointer, not owned
      stopped_execution_.address = other.stopped_execution_.address;
      stopped_execution_.must_be_valid =
          std::make_pair(other.stopped_execution_.must_be_valid.first.clone(),
                         other.stopped_execution_.must_be_valid.second);
      stopped_execution_.calling_context =
          other.stopped_execution_.calling_context;
      stopped_execution_.specialized_type =
          other.stopped_execution_.specialized_type;
      stopped_execution_.type_trace =
          other.stopped_execution_.type_trace.clone();
    } else {
      stopped_execution_.reset(); // Reset to default
    }
  }

  ExecutionDomain &operator=(const ExecutionDomain &other) {
    if (this != &other) {
      variant_ = other.variant_;
      astate_ = other.astate_
                    ? std::make_unique<AbductiveDomain>(other.astate_->clone())
                    : nullptr;
      entry_pred_ = other.entry_pred_;
      if (other.variant_ == Variant::Stopped) {
        stopped_execution_.kind = other.stopped_execution_.kind;
        if (other.stopped_execution_.astate) {
          stopped_execution_.astate = std::make_unique<AbductiveDomain>(
              other.stopped_execution_.astate->clone());
        }
        stopped_execution_.return_value = other.stopped_execution_.return_value;
        stopped_execution_.diagnostic = other.stopped_execution_.diagnostic;
        stopped_execution_.trace_to_issue =
            other.stopped_execution_.trace_to_issue.clone();
        stopped_execution_.latent_issue = other.stopped_execution_.latent_issue;
        stopped_execution_.address = other.stopped_execution_.address;
        stopped_execution_.must_be_valid =
            std::make_pair(other.stopped_execution_.must_be_valid.first.clone(),
                           other.stopped_execution_.must_be_valid.second);
        stopped_execution_.calling_context =
            other.stopped_execution_.calling_context;
        stopped_execution_.specialized_type =
            other.stopped_execution_.specialized_type;
        stopped_execution_.type_trace =
            other.stopped_execution_.type_trace.clone();
      } else {
        stopped_execution_.reset(); // Reset to default
      }
    }
    return *this;
  }

  // Factory methods for different variants
  static ExecutionDomain
  continueProgram(std::unique_ptr<AbductiveDomain> astate) {
    ExecutionDomain d;
    d.variant_ = Variant::ContinueProgram;
    d.astate_ = std::move(astate);
    return d;
  }

  static ExecutionDomain
  exceptionRaised(std::unique_ptr<AbductiveDomain> astate) {
    ExecutionDomain d;
    d.variant_ = Variant::ExceptionRaised;
    d.astate_ = std::move(astate);
    return d;
  }

  static ExecutionDomain exitProgram(std::unique_ptr<AbductiveDomain> astate,
                                     llvm::Optional<AbstractValue> ret_val =
                                         llvm::None) {
    ExecutionDomain d;
    d.variant_ = Variant::Stopped;
    d.stopped_execution_.kind = StoppedExecutionKind::ExitProgram;
    d.stopped_execution_.astate = std::move(astate);
    d.stopped_execution_.return_value = ret_val;
    return d;
  }

  static ExecutionDomain abortProgram(std::unique_ptr<AbductiveDomain> astate,
                                      OperationResult diagnostic, Trace trace) {
    ExecutionDomain d;
    d.variant_ = Variant::Stopped;
    d.stopped_execution_.kind = StoppedExecutionKind::AbortProgram;
    d.stopped_execution_.astate = std::move(astate);
    d.stopped_execution_.diagnostic = diagnostic;
    d.stopped_execution_.trace_to_issue = std::move(trace);
    return d;
  }

  static ExecutionDomain
  latentAbortProgram(std::unique_ptr<AbductiveDomain> astate,
                     LatentIssue *latent_issue) {
    ExecutionDomain d;
    d.variant_ = Variant::Stopped;
    d.stopped_execution_.kind = StoppedExecutionKind::LatentAbortProgram;
    d.stopped_execution_.astate = std::move(astate);
    d.stopped_execution_.latent_issue = latent_issue;
    return d;
  }

  static ExecutionDomain latentInvalidAccess(
      std::unique_ptr<AbductiveDomain> astate, AbstractValue address,
      std::pair<Trace, llvm::Optional<InvalidationKind>> must_be_valid,
      std::vector<std::pair<const llvm::Function *, const llvm::Instruction *>>
          calling_context) {
    ExecutionDomain d;
    d.variant_ = Variant::Stopped;
    d.stopped_execution_.kind = StoppedExecutionKind::LatentInvalidAccess;
    d.stopped_execution_.astate = std::move(astate);
    d.stopped_execution_.address = address;
    d.stopped_execution_.must_be_valid = std::move(must_be_valid);
    d.stopped_execution_.calling_context = std::move(calling_context);
    return d;
  }

  static ExecutionDomain
  latentSpecializedTypeIssue(std::unique_ptr<AbductiveDomain> astate,
                             std::string specialized_type, Trace type_trace) {
    ExecutionDomain d;
    d.variant_ = Variant::Stopped;
    d.stopped_execution_.kind =
        StoppedExecutionKind::LatentSpecializedTypeIssue;
    d.stopped_execution_.astate = std::move(astate);
    d.stopped_execution_.specialized_type = std::move(specialized_type);
    d.stopped_execution_.type_trace = std::move(type_trace);
    return d;
  }

  // Accessors
  bool isContinueProgram() const {
    return variant_ == Variant::ContinueProgram;
  }
  bool isExceptionRaised() const {
    return variant_ == Variant::ExceptionRaised;
  }
  bool isStopped() const { return variant_ == Variant::Stopped; }

  bool isExitProgram() const {
    return isStopped() &&
           stopped_execution_.kind == StoppedExecutionKind::ExitProgram;
  }

  bool isAbortProgram() const {
    return isStopped() &&
           stopped_execution_.kind == StoppedExecutionKind::AbortProgram;
  }

  bool isLatentAbortProgram() const {
    return isStopped() &&
           stopped_execution_.kind == StoppedExecutionKind::LatentAbortProgram;
  }

  bool isLatentInvalidAccess() const {
    return isStopped() &&
           stopped_execution_.kind == StoppedExecutionKind::LatentInvalidAccess;
  }

  bool isLatentSpecializedTypeIssue() const {
    return isStopped() && stopped_execution_.kind ==
                              StoppedExecutionKind::LatentSpecializedTypeIssue;
  }

  // Legacy compatibility
  bool isContinue() const { return isContinueProgram(); }

  // CFG predecessor used to enter the current basic block (for sound PHI).
  const llvm::BasicBlock *getEntryPred() const { return entry_pred_; }
  void setEntryPred(const llvm::BasicBlock *pred) { entry_pred_ = pred; }

  AbductiveDomain *getAstate() {
    return isStopped() ? stopped_execution_.astate.get() : astate_.get();
  }
  const AbductiveDomain *getAstate() const {
    return isStopped() ? stopped_execution_.astate.get() : astate_.get();
  }

  const StoppedExecution &getStoppedExecution() const {
    return stopped_execution_;
  }
  StoppedExecution &getStoppedExecution() { return stopped_execution_; }

  // Set state (legacy compatibility)
  void setState(ExecutionState s) {
    if (s == ExecutionState::ContinueProgram) {
      variant_ = Variant::ContinueProgram;
    } else if (s == ExecutionState::ExceptionRaised) {
      variant_ = Variant::ExceptionRaised;
    } else {
      variant_ = Variant::Stopped;
      stopped_execution_.kind = StoppedExecutionKind::ExitProgram;
      stopped_execution_.return_value = llvm::None;
    }
  }

  // Make ExecutionDomain movable for use in containers
  ExecutionDomain(ExecutionDomain &&) noexcept = default;
  ExecutionDomain &operator=(ExecutionDomain &&) noexcept = default;

  ExecutionDomain clone() const {
    ExecutionDomain c;
    c.variant_ = variant_;
    c.astate_ =
        astate_ ? std::make_unique<AbductiveDomain>(astate_->clone()) : nullptr;
    c.entry_pred_ = entry_pred_;
    if (stopped_execution_.astate) {
      c.stopped_execution_.astate =
          std::make_unique<AbductiveDomain>(stopped_execution_.astate->clone());
    }
    c.stopped_execution_.kind = stopped_execution_.kind;
    c.stopped_execution_.return_value = stopped_execution_.return_value;
    c.stopped_execution_.diagnostic = stopped_execution_.diagnostic;
    c.stopped_execution_.trace_to_issue =
        stopped_execution_.trace_to_issue.clone();
    c.stopped_execution_.latent_issue =
        stopped_execution_.latent_issue; // Pointer, not owned
    c.stopped_execution_.address = stopped_execution_.address;
    c.stopped_execution_.must_be_valid =
        std::make_pair(stopped_execution_.must_be_valid.first.clone(),
                       stopped_execution_.must_be_valid.second);
    c.stopped_execution_.calling_context = stopped_execution_.calling_context;
    c.stopped_execution_.specialized_type = stopped_execution_.specialized_type;
    c.stopped_execution_.type_trace = stopped_execution_.type_trace.clone();
    return c;
  }
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEEXECUTIONDOMAIN_H
