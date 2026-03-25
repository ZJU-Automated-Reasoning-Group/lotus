#include "Checker/Pulse/Interproc/PulseSummary.h"

#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

namespace pulse {

//===----------------------------------------------------------------------===//
// Summaries
//
// A summary captures (possibly multiple) witness behaviors of a function.
//
// Sound incorrectness note:
// - Disjunctive entries should preserve *existential* behaviors (there exists a
//   path satisfying the entry). When compressing/merging, be careful not to
//   conjoin path conditions, which can drop feasible witnesses.
// - Latent issues are carried in summaries so callers can make them manifest.
//===----------------------------------------------------------------------===//

// SummaryEntry implementation
SummaryEntry::SummaryEntry(std::unique_ptr<AbductiveDomain> pre,
                           PulseFormula pre_formula,
                           std::unique_ptr<AbductiveDomain> post,
                           PulseFormula post_formula,
                           llvm::Optional<AbstractValue> ret_val,
                           llvm::Optional<LatentIssueSummary> latent_issue)
    : pre_(std::move(pre)), pre_formula_(std::move(pre_formula)),
      post_(std::move(post)), post_formula_(std::move(post_formula)),
      return_value_(ret_val), latent_issue_(std::move(latent_issue)) {}

SummaryEntry SummaryEntry::clone() const {
  llvm::Optional<LatentIssueSummary> latent;
  if (latent_issue_) {
    LatentIssueSummary copied;
    copied.diagnostic = latent_issue_->diagnostic;
    copied.address = latent_issue_->address;
    copied.trace = latent_issue_->trace.clone();
    copied.calling_context = latent_issue_->calling_context;
    latent = std::move(copied);
  }
  return SummaryEntry(
      pre_ ? std::make_unique<AbductiveDomain>(pre_->clone()) : nullptr,
      pre_formula_.clone(),
      post_ ? std::make_unique<AbductiveDomain>(post_->clone()) : nullptr,
      post_formula_.clone(), return_value_, std::move(latent));
}

// PulseSummary implementation
PulseSummary::PulseSummary(const llvm::Function *F) : function_(F) {}

PulseSummary::PulseSummary(const llvm::Function *F,
                           std::unique_ptr<AbductiveDomain> pre,
                           PulseFormula pre_formula,
                           std::unique_ptr<AbductiveDomain> post,
                           PulseFormula post_formula,
                           llvm::Optional<AbstractValue> ret_val)
    : function_(F) {
  // Create a single pre/post entry
  pre_post_list_.emplace_back(std::move(pre), std::move(pre_formula),
                              std::move(post), std::move(post_formula),
                              ret_val);
}

llvm::Optional<AbstractValue>
PulseSummary::getFormalAV(const llvm::Value *formal) const {
  auto it = formal_to_av_.find(formal);
  if (it != formal_to_av_.end()) {
    return it->second;
  }
  return llvm::None;
}

void PulseSummary::setFormalAV(const llvm::Value *formal, AbstractValue av) {
  formal_to_av_[formal] = av;
}

void PulseSummary::addPrePost(SummaryEntry entry) {
  pre_post_list_.push_back(std::move(entry));
}

// Legacy accessors for backward compatibility
const AbductiveDomain *PulseSummary::getPre() const {
  if (pre_post_list_.empty()) {
    return nullptr;
  }
  return pre_post_list_[0].getPre();
}

const AbductiveDomain *PulseSummary::getPost() const {
  if (pre_post_list_.empty()) {
    return nullptr;
  }
  return pre_post_list_[0].getPost();
}

const PulseFormula &PulseSummary::getPreFormula() const {
  static PulseFormula empty;
  if (pre_post_list_.empty()) {
    return empty;
  }
  return pre_post_list_[0].getPreFormula();
}

const PulseFormula &PulseSummary::getPostFormula() const {
  static PulseFormula empty;
  if (pre_post_list_.empty()) {
    return empty;
  }
  return pre_post_list_[0].getPostFormula();
}

llvm::Optional<AbstractValue> PulseSummary::getReturnValue() const {
  if (pre_post_list_.empty()) {
    return llvm::None;
  }
  return pre_post_list_[0].getReturnValue();
}

PulseSummary PulseSummary::join(const PulseSummary &s1,
                                const PulseSummary &s2) {
  PulseSummary joined(s1.function_);

  // Merge pre/post lists - clone entries since they're not copyable
  for (const auto &entry : s1.pre_post_list_) {
    joined.pre_post_list_.push_back(entry.clone());
  }
  for (const auto &entry : s2.pre_post_list_) {
    joined.pre_post_list_.push_back(entry.clone());
  }

  // Merge non-disjunctive summaries
  joined.non_disj_ = NonDisjunctiveSummary::join(s1.non_disj_, s2.non_disj_);

  // Merge formal mappings
  joined.formal_to_av_ = s1.formal_to_av_;
  for (const auto &kv : s2.formal_to_av_) {
    if (joined.formal_to_av_.find(kv.first) == joined.formal_to_av_.end()) {
      joined.formal_to_av_[kv.first] = kv.second;
    }
  }

  return joined;
}

// SummaryManager implementation

void SummaryManager::storeSummary(const llvm::Function *F,
                                  PulseSummary summary) {
  summaries_.erase(F);
  summaries_.emplace(F, std::move(summary));
}

const PulseSummary *SummaryManager::getSummary(const llvm::Function *F) const {
  auto it = summaries_.find(F);
  if (it != summaries_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool SummaryManager::hasSummary(const llvm::Function *F) const {
  return summaries_.find(F) != summaries_.end();
}

void SummaryManager::clear() {
  summaries_.clear();
  skipped_calls_.clear();
}

} // namespace pulse
