#ifndef CHECKER_PULSE_PULSENONDISJUNCTIVEDOMAIN_H
#define CHECKER_PULSE_PULSENONDISJUNCTIVEDOMAIN_H

#include "Checker/Pulse/Domain/PulseDomain.h"

#include <map>
#include <memory>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace llvm {
class Function;
class Argument;
class StoreInst;
} // namespace llvm

namespace pulse {

/**
 * NonDisjunctiveDomain: efficient non-disjunctive representation
 * for joining states at merge points.
 * Tracks copied stores and const-refable params (Infer-aligned).
 */
class NonDisjunctiveDomain {
private:
  std::unique_ptr<AbductiveDomain> summary_;
  std::vector<const llvm::StoreInst *> copied_stores_;
  std::vector<const llvm::Argument *> const_refable_params_;

public:
  NonDisjunctiveDomain() = default;

  void addState(const AbductiveDomain &state);

  const AbductiveDomain *getSummary() const { return summary_.get(); }
  AbductiveDomain *getSummary() { return summary_.get(); }

  bool isEmpty() const { return summary_ == nullptr; }

  void clear() {
    summary_.reset();
    copied_stores_.clear();
    const_refable_params_.clear();
  }

  void join(const NonDisjunctiveDomain &other);

  void recordCopy(const llvm::StoreInst *SI) { copied_stores_.push_back(SI); }
  void recordConstRefableParam(const llvm::Argument *A) {
    const_refable_params_.push_back(A);
  }

  const std::vector<const llvm::StoreInst *> &getCopiedStores() const {
    return copied_stores_;
  }
  const std::vector<const llvm::Argument *> &getConstRefableParams() const {
    return const_refable_params_;
  }
};

/**
 * NonDisjunctiveSummary: summary component for non-disjunctive part
 */
class NonDisjunctiveSummary {
private:
  std::unique_ptr<AbductiveDomain> summary_;

public:
  NonDisjunctiveSummary() = default;

  explicit NonDisjunctiveSummary(std::unique_ptr<AbductiveDomain> summary)
      : summary_(std::move(summary)) {}

  // Move constructor and assignment
  NonDisjunctiveSummary(NonDisjunctiveSummary &&) = default;
  NonDisjunctiveSummary &operator=(NonDisjunctiveSummary &&) = default;

  // Copy constructor and assignment (for join operations)
  NonDisjunctiveSummary(const NonDisjunctiveSummary &other)
      : summary_(other.summary_ ? std::make_unique<AbductiveDomain>(
                                      other.summary_->clone())
                                : nullptr) {}

  NonDisjunctiveSummary &operator=(const NonDisjunctiveSummary &other) {
    if (this != &other) {
      summary_ =
          other.summary_
              ? std::make_unique<AbductiveDomain>(other.summary_->clone())
              : nullptr;
    }
    return *this;
  }

  const AbductiveDomain *getSummary() const { return summary_.get(); }
  AbductiveDomain *getSummary() { return summary_.get(); }

  bool isEmpty() const { return summary_ == nullptr; }

  NonDisjunctiveSummary clone() const {
    if (summary_) {
      return NonDisjunctiveSummary(
          std::make_unique<AbductiveDomain>(summary_->clone()));
    }
    return NonDisjunctiveSummary();
  }

  /**
   * Join two non-disjunctive summaries
   */
  static NonDisjunctiveSummary join(const NonDisjunctiveSummary &s1,
                                    const NonDisjunctiveSummary &s2);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSENONDISJUNCTIVEDOMAIN_H
