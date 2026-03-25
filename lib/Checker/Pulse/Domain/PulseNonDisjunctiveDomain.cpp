#include "Checker/Pulse/Domain/PulseNonDisjunctiveDomain.h"

namespace pulse {

//===----------------------------------------------------------------------===//
// NonDisjunctiveDomain
//
// Tracks "must" information aggregated across disjuncts for efficiency. This
// is intentionally an *intersection*-like abstraction: it keeps only facts that
// are common to all observed states. This is useful for diagnostics such as
// unnecessary copies / const-ref suggestions.
//
// Note: This component is not where bug witnessability is decided; it is used
// for non-bug reports and summarization.
//===----------------------------------------------------------------------===//

void NonDisjunctiveDomain::addState(const AbductiveDomain &state) {
  if (!summary_) {
    // First state: use it as the summary
    summary_ = std::make_unique<AbductiveDomain>(state.clone());
    return;
  }

  // Compute intersection: keep only common parts
  // This is simplified - full implementation would do proper intersection
  AbductiveDomain intersection;

  // Intersect stacks: keep variables that exist in both
  for (const auto &kv : state.getPostStack().getMap()) {
    if (summary_->getPostStack().find(kv.first)) {
      // Variable exists in both - keep it
      intersection.getPostStack().add(kv.first, kv.second);
    }
  }

  // Intersect heaps: keep edges that exist in both
  for (const auto &kv : state.getPostHeap().getEdges()) {
    auto summary_it = summary_->getPostHeap().getEdges().find(kv.first);
    if (summary_it != summary_->getPostHeap().getEdges().end()) {
      for (const auto &edge_kv : kv.second) {
        if (summary_it->second.find(edge_kv.first) !=
            summary_it->second.end()) {
          // Edge exists in both - keep it
          intersection.getPostHeap().addEdge(kv.first, edge_kv.first,
                                             edge_kv.second);
        }
      }
    }
  }

  // Intersect attributes: keep attributes that exist in both
  for (const auto &kv : state.getPostAttrs().getAttrs()) {
    if (summary_->getPostAttrs().has(kv.first, Attribute::Allocated)) {
      // Keep common attributes
      AttributeSet common;
      const auto &summary_attrs = summary_->getPostAttrs().get(kv.first);
      for (Attribute attr : kv.second) {
        if (summary_attrs.count(attr) > 0) {
          common.insert(attr);
        }
      }
      for (Attribute attr : common) {
        intersection.getPostAttrs().add(kv.first, attr);
      }
    }
  }

  summary_ = std::make_unique<AbductiveDomain>(std::move(intersection));
}

void NonDisjunctiveDomain::join(const NonDisjunctiveDomain &other) {
  if (other.isEmpty()) {
    return;
  }
  if (isEmpty()) {
    summary_ = std::make_unique<AbductiveDomain>(other.summary_->clone());
    copied_stores_ = other.copied_stores_;
    const_refable_params_ = other.const_refable_params_;
    return;
  }
  addState(*other.summary_);
  for (const auto *S : other.copied_stores_)
    copied_stores_.push_back(S);
  for (const auto *A : other.const_refable_params_)
    const_refable_params_.push_back(A);
}

NonDisjunctiveSummary
NonDisjunctiveSummary::join(const NonDisjunctiveSummary &s1,
                            const NonDisjunctiveSummary &s2) {
  if (s1.isEmpty()) {
    return s2.clone();
  }
  if (s2.isEmpty()) {
    return s1.clone();
  }

  // Compute intersection
  NonDisjunctiveDomain domain;
  domain.addState(*s1.getSummary());
  domain.addState(*s2.getSummary());

  if (domain.isEmpty()) {
    return NonDisjunctiveSummary();
  }

  return NonDisjunctiveSummary(
      std::make_unique<AbductiveDomain>(domain.getSummary()->clone()));
}

} // namespace pulse
