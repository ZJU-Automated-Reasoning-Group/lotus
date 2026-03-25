#ifndef CHECKER_PULSE_PULSESUMMARY_H
#define CHECKER_PULSE_PULSESUMMARY_H

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

#include <map>
#include <memory>
#include <vector>

#include <llvm/IR/Function.h>

namespace llvm {
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace pulse {

/**
 * SummaryEntry: represents a single pre/post pair in a disjunctive summary
 */
class SummaryEntry {
public:
  struct LatentIssueSummary {
    OperationResult diagnostic;
    AbstractValue address;
    Trace trace;
    std::vector<std::pair<const llvm::Function *, const llvm::Instruction *>>
        calling_context;
  };

private:
  std::unique_ptr<AbductiveDomain> pre_;
  PulseFormula pre_formula_;
  std::unique_ptr<AbductiveDomain> post_;
  PulseFormula post_formula_;
  llvm::Optional<AbstractValue> return_value_;

  llvm::Optional<LatentIssueSummary> latent_issue_;

public:
  SummaryEntry(std::unique_ptr<AbductiveDomain> pre, PulseFormula pre_formula,
               std::unique_ptr<AbductiveDomain> post, PulseFormula post_formula,
               llvm::Optional<AbstractValue> ret_val,
               llvm::Optional<LatentIssueSummary> latent_issue = llvm::None);

  // Move constructor and assignment
  SummaryEntry(SummaryEntry &&) = default;
  SummaryEntry &operator=(SummaryEntry &&) = default;

  // Delete copy constructor and assignment (use clone() instead)
  SummaryEntry(const SummaryEntry &) = delete;
  SummaryEntry &operator=(const SummaryEntry &) = delete;

  const AbductiveDomain *getPre() const { return pre_.get(); }
  const AbductiveDomain *getPost() const { return post_.get(); }
  const PulseFormula &getPreFormula() const { return pre_formula_; }
  const PulseFormula &getPostFormula() const { return post_formula_; }
  llvm::Optional<AbstractValue> getReturnValue() const { return return_value_; }
  const llvm::Optional<LatentIssueSummary> &getLatentIssue() const {
    return latent_issue_;
  }

  SummaryEntry clone() const;
};

/**
 * PulseSummary: represents a function summary for interprocedural analysis.
 * Contains disjunctive pre/post pairs and a non-disjunctive component.
 */
class PulseSummary {
private:
  const llvm::Function *function_;

  // Disjunctive summary: list of pre/post pairs
  std::vector<SummaryEntry> pre_post_list_;

  // Non-disjunctive summary component
  NonDisjunctiveSummary non_disj_;

  // Mapping from formal parameters to their abstract values in the summary
  std::map<const llvm::Value *, AbstractValue> formal_to_av_;

public:
  PulseSummary(const llvm::Function *F);

  // Legacy constructor for single pre/post pair
  PulseSummary(const llvm::Function *F, std::unique_ptr<AbductiveDomain> pre,
               PulseFormula pre_formula, std::unique_ptr<AbductiveDomain> post,
               PulseFormula post_formula,
               llvm::Optional<AbstractValue> ret_val);

  const llvm::Function *getFunction() const { return function_; }

  // Disjunctive summary access
  const std::vector<SummaryEntry> &getPrePostList() const {
    return pre_post_list_;
  }
  void addPrePost(SummaryEntry entry);

  // Move constructor and assignment
  PulseSummary(PulseSummary &&) = default;
  PulseSummary &operator=(PulseSummary &&) = default;

  // Delete copy constructor and assignment (use join() or clone if needed)
  PulseSummary(const PulseSummary &) = delete;
  PulseSummary &operator=(const PulseSummary &) = delete;

  // Non-disjunctive summary access
  const NonDisjunctiveSummary &getNonDisj() const { return non_disj_; }
  NonDisjunctiveSummary &getNonDisj() { return non_disj_; }
  void setNonDisj(NonDisjunctiveSummary non_disj) {
    non_disj_ = std::move(non_disj);
  }

  // Legacy accessors (for backward compatibility)
  const AbductiveDomain *getPre() const;
  const AbductiveDomain *getPost() const;
  const PulseFormula &getPreFormula() const;
  const PulseFormula &getPostFormula() const;
  llvm::Optional<AbstractValue> getReturnValue() const;

  /**
   * Get abstract value for a formal parameter
   */
  llvm::Optional<AbstractValue> getFormalAV(const llvm::Value *formal) const;

  /**
   * Set mapping from formal to abstract value
   */
  void setFormalAV(const llvm::Value *formal, AbstractValue av);

  /**
   * Check if summary is valid (non-empty)
   */
  bool isValid() const {
    return !pre_post_list_.empty() || !non_disj_.isEmpty();
  }

  /**
   * Join two summaries
   */
  static PulseSummary join(const PulseSummary &s1, const PulseSummary &s2);
};

/**
 * SkippedCalls: tracks function calls for which no summary was found
 */
class SkippedCalls {
private:
  struct SkippedCall {
    const llvm::Function *function;
    const llvm::Instruction *call_site;
    const llvm::Function *caller;

    SkippedCall(const llvm::Function *f, const llvm::Instruction *cs,
                const llvm::Function *c)
        : function(f), call_site(cs), caller(c) {}
  };

  std::vector<SkippedCall> skipped_calls_;

public:
  SkippedCalls() = default;

  void addSkippedCall(const llvm::Function *func,
                      const llvm::Instruction *call_site,
                      const llvm::Function *caller) {
    skipped_calls_.emplace_back(func, call_site, caller);
  }

  const std::vector<SkippedCall> &getSkippedCalls() const {
    return skipped_calls_;
  }
  bool isEmpty() const { return skipped_calls_.empty(); }
  size_t size() const { return skipped_calls_.size(); }

  void clear() { skipped_calls_.clear(); }

  SkippedCalls clone() const {
    SkippedCalls cloned;
    cloned.skipped_calls_ = skipped_calls_;
    return cloned;
  }
};

/**
 * SummaryManager: manages function summaries for interprocedural analysis
 */
class SummaryManager {
private:
  std::map<const llvm::Function *, PulseSummary> summaries_;
  SkippedCalls skipped_calls_;

public:
  /**
   * Store a summary for a function
   */
  void storeSummary(const llvm::Function *F, PulseSummary summary);

  /**
   * Get summary for a function (if available)
   */
  const PulseSummary *getSummary(const llvm::Function *F) const;

  /**
   * Check if summary exists for function
   */
  bool hasSummary(const llvm::Function *F) const;

  /**
   * Record a skipped call (no summary found)
   */
  void recordSkippedCall(const llvm::Function *func,
                         const llvm::Instruction *call_site,
                         const llvm::Function *caller) {
    skipped_calls_.addSkippedCall(func, call_site, caller);
  }

  /**
   * Get skipped calls
   */
  const SkippedCalls &getSkippedCalls() const { return skipped_calls_; }

  /**
   * Clear all summaries and skipped calls
   */
  void clear();
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSESUMMARY_H
