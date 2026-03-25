#ifndef CHECKER_PULSE_PULSEDIAGNOSTIC_H
#define CHECKER_PULSE_PULSEDIAGNOSTIC_H

#include "Checker/Pulse/Core/PulseAbstractValue.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"

#include <optional>
#include <string>
#include <vector>

#include <llvm/IR/Instruction.h>

namespace pulse {

// Issue Type Constants (Aligned with Infer)
namespace IssueType {
constexpr const char *ResourceLeak = "Resource Leak";
constexpr const char *NullDereference = "Null Pointer Dereference";
constexpr const char *UseAfterFree = "Use After Free";
constexpr const char *OutOfBounds = "Out Of Bounds Access";
constexpr const char *InvalidFree = "Invalid Free";
constexpr const char *UninitializedRead = "Uninitialized Read";
constexpr const char *TaintError = "Taint Error";
constexpr const char *UnnecessaryCopy = "Unnecessary Copy";
constexpr const char *StackVariableAddressEscape =
    "Stack Variable Address Escape";
} // namespace IssueType

class Diagnostic {
public:
  virtual ~Diagnostic() = default;

  virtual std::string getMessage() const = 0;
  virtual std::string getDescription() const = 0;
  virtual std::string getSuggestion() const = 0;
  virtual std::string getIssueType() const = 0;
  virtual const llvm::Instruction *getLocation() const = 0;
  virtual const Trace *getTrace() const { return nullptr; }

  // For deduplication
  virtual size_t getHash() const = 0;
  virtual bool equals(const Diagnostic &other) const = 0;
};

// Access to invalid address (Null deref, Use-after-free)
class AccessToInvalidAddress : public Diagnostic {
  const llvm::Instruction *location_;
  std::string message_;
  std::string description_;
  std::string suggestion_;
  std::string issue_type_;
  Trace trace_;
  InvalidationKind invalidation_kind_; // For specialized reporting

public:
  AccessToInvalidAddress(const llvm::Instruction *loc, const std::string &msg,
                         const std::string &desc, const std::string &sugg,
                         const std::string &type, Trace trace,
                         InvalidationKind invKind = InvalidationKind::Other)
      : location_(loc), message_(msg), description_(desc), suggestion_(sugg),
        issue_type_(type), trace_(std::move(trace)),
        invalidation_kind_(invKind) {}

  std::string getMessage() const override { return message_; }
  std::string getDescription() const override { return description_; }
  std::string getSuggestion() const override { return suggestion_; }
  std::string getIssueType() const override { return issue_type_; }
  const llvm::Instruction *getLocation() const override { return location_; }
  const Trace *getTrace() const override { return &trace_; }

  size_t getHash() const override;
  bool equals(const Diagnostic &other) const override;
};

class ResourceLeak : public Diagnostic {
  const llvm::Instruction *location_;
  const llvm::Instruction *allocation_site_;
  std::string resource_name_; // e.g., "File", "Socket"
  Trace trace_;

public:
  ResourceLeak(const llvm::Instruction *loc,
               const llvm::Instruction *alloc_site, const std::string &resource,
               Trace trace)
      : location_(loc), allocation_site_(alloc_site), resource_name_(resource),
        trace_(std::move(trace)) {}

  std::string getMessage() const override;
  std::string getDescription() const override;
  std::string getSuggestion() const override {
    return "Close the resource before it goes out of scope.";
  }
  std::string getIssueType() const override { return IssueType::ResourceLeak; }
  const llvm::Instruction *getLocation() const override { return location_; }
  const Trace *getTrace() const override { return &trace_; }

  size_t getHash() const override;
  bool equals(const Diagnostic &other) const override;
};

class TaintFlow : public Diagnostic {
  const llvm::Instruction *location_;
  std::string source_kind_;
  std::string sink_kind_;
  Trace trace_;

public:
  TaintFlow(const llvm::Instruction *loc, const std::string &source,
            const std::string &sink, Trace trace)
      : location_(loc), source_kind_(source), sink_kind_(sink),
        trace_(std::move(trace)) {}

  std::string getMessage() const override;
  std::string getDescription() const override;
  std::string getSuggestion() const override {
    return "Validate untrusted input before using it in sensitive operations.";
  }
  std::string getIssueType() const override { return IssueType::TaintError; }
  const llvm::Instruction *getLocation() const override { return location_; }
  const Trace *getTrace() const override { return &trace_; }

  size_t getHash() const override;
  bool equals(const Diagnostic &other) const override;
};

class UnnecessaryCopy : public Diagnostic {
  const llvm::Instruction *location_;
  std::string variable_name_;
  std::string type_name_;

public:
  UnnecessaryCopy(const llvm::Instruction *loc, const std::string &var,
                  const std::string &type)
      : location_(loc), variable_name_(var), type_name_(type) {}

  std::string getMessage() const override;
  std::string getDescription() const override;
  std::string getSuggestion() const override;
  std::string getIssueType() const override {
    return IssueType::UnnecessaryCopy;
  }
  const llvm::Instruction *getLocation() const override { return location_; }

  size_t getHash() const override;
  bool equals(const Diagnostic &other) const override;
};

/**
 * StackVariableAddressEscape: a stack-derived address is returned or stored
 * into non-stack memory (global/heap), making it invalid to use outside its
 * scope.
 *
 * This is intended for sound incorrectness reporting: only emit when the
 * stack-derived provenance is proven (via `Attribute::Stack`).
 */
class StackVariableAddressEscape : public Diagnostic {
  const llvm::Instruction *location_;
  AbstractValue address_;
  std::string message_;
  std::string suggestion_;
  Trace trace_;

public:
  StackVariableAddressEscape(const llvm::Instruction *loc, AbstractValue addr,
                             const std::string &msg, const std::string &sugg,
                             Trace trace)
      : location_(loc), address_(addr), message_(msg), suggestion_(sugg),
        trace_(std::move(trace)) {}

  std::string getMessage() const override { return message_; }
  std::string getDescription() const override {
    return "Invalid use of stack address: it escapes its scope.";
  }
  std::string getSuggestion() const override { return suggestion_; }
  std::string getIssueType() const override {
    return IssueType::StackVariableAddressEscape;
  }
  const llvm::Instruction *getLocation() const override { return location_; }
  const Trace *getTrace() const override { return &trace_; }

  size_t getHash() const override;
  bool equals(const Diagnostic &other) const override;
};

/**
 * InvalidFree: freeing a pointer that is not a heap allocation (e.g., stack or
 * global storage).
 *
 * Sound incorrectness: only emit when non-heap provenance is proven via
 * `Attribute::{Stack,Global}`.
 *
 * Note: we intentionally stop exploring the current path after reporting,
 * because undefined behavior makes subsequent reports unreliable.
 */
class InvalidFree : public Diagnostic {
  const llvm::Instruction *location_;
  AbstractValue address_;
  std::string message_;
  std::string suggestion_;
  Trace trace_;

public:
  InvalidFree(const llvm::Instruction *loc, AbstractValue addr,
              const std::string &msg, const std::string &sugg, Trace trace)
      : location_(loc), address_(addr), message_(msg), suggestion_(sugg),
        trace_(std::move(trace)) {}

  std::string getMessage() const override { return message_; }
  std::string getDescription() const override {
    return "Freeing memory that is not on the heap causes undefined behavior.";
  }
  std::string getSuggestion() const override { return suggestion_; }
  std::string getIssueType() const override { return IssueType::InvalidFree; }
  const llvm::Instruction *getLocation() const override { return location_; }
  const Trace *getTrace() const override { return &trace_; }

  size_t getHash() const override;
  bool equals(const Diagnostic &other) const override;
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEDIAGNOSTIC_H
