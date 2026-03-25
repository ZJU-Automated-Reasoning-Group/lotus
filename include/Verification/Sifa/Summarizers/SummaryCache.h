//===-- Verification/Sifa/Summarizers/SummaryCache.h
//-----------------------===//
//
// Standalone re-use summary cache (ported from Ultimate Library-Sifa).
//
// Ultimate SummaryCache is not an ICallSummarizer: it stores (input, summary)
// and reUseOrCompute(input, isSubsetEq, computeSummary, tools) finds cached
// entries where input ⊆ knownInput, returns meet(supersets) or compute & cache.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_SUMMARYCACHE_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_SUMMARYCACHE_H

#include <functional>
#include <utility>
#include <vector>

namespace lotus {
namespace sifa {

/// Standalone cache for one procedure or loop: (input, summary). Re-use when
/// input ⊆ knownInput; return meet(supersets) or compute and cache.
template <typename StateT> class SummaryCache {
public:
  using Entry = std::pair<StateT, StateT>;
  using IsSubsetEqFn = std::function<bool(const StateT &, const StateT &)>;
  using ComputeSummaryFn = std::function<StateT()>;
  using MeetFn = std::function<StateT(const StateT &, const StateT &)>;

  std::vector<Entry> reusableEntries(const StateT &input,
                                     IsSubsetEqFn isSubsetEq) const {
    std::vector<Entry> supersets;
    for (const auto &p : knownSummaries_) {
      if (isSubsetEq(input, p.first))
        supersets.push_back(p);
    }
    return supersets;
  }

  std::vector<StateT> reusableSummaries(const StateT &input,
                                        IsSubsetEqFn isSubsetEq) const {
    std::vector<StateT> supersets;
    for (const auto &p : reusableEntries(input, std::move(isSubsetEq))) {
      supersets.push_back(p.second);
    }
    return supersets;
  }

  void store(const StateT &input, const StateT &summary) {
    knownSummaries_.emplace_back(input, summary);
  }

  /// Re-use cached summary when isSubsetEq(input, knownInput); return meet of
  /// such summaries. Else compute via computeSummary, cache, and return.
  StateT reUseOrCompute(const StateT &input, IsSubsetEqFn isSubsetEq,
                        ComputeSummaryFn computeSummary, MeetFn meetFn) {
    std::vector<StateT> supersets =
        reusableSummaries(input, std::move(isSubsetEq));
    if (!supersets.empty()) {
      StateT acc = supersets.front();
      for (std::size_t i = 1; i < supersets.size(); ++i)
        acc = meetFn(acc, supersets[i]);
      return acc;
    }
    StateT summary = computeSummary();
    store(input, summary);
    return summary;
  }

private:
  std::vector<Entry> knownSummaries_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_SUMMARYCACHE_H
