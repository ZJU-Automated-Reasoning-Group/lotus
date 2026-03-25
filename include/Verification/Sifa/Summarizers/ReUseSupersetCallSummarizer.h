//===-- Verification/Sifa/Summarizers/ReUseSupersetCallSummarizer.h
//--------===//
//
// Call summarizer that re-uses summaries when input ⊆ knownInput
// (Ultimate-aligned).
//
// Uses SummaryCache per callee; reUseOrCompute: if ∃ cached (knownInput,
// summary) with leq(input, knownInput), return meet of such summaries; else
// compute and cache.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_REUSESUPERSETCALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_REUSESUPERSETCALLSUMMARIZER_H

#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"
#include "Verification/Sifa/Summarizers/SummaryCache.h"

#include <algorithm>
#include <unordered_map>

namespace lotus {
namespace sifa {

template <typename LabelT, typename StateT>
class ReUseSupersetCallSummarizer final : public ICallSummarizer<StateT> {
public:
  using Domain = AbstractDomain<LabelT, StateT>;

  ReUseSupersetCallSummarizer(const Domain &domain,
                              ICallSummarizer<StateT> &inner)
      : domain_(domain), inner_(inner) {}

  /// Ultimate-aligned: optional stats for CALL_SUMMARIZER_OVERALL_TIME and
  /// APPLICATIONS.
  ReUseSupersetCallSummarizer(SifaStats &stats, const Domain &domain,
                              ICallSummarizer<StateT> &inner)
      : stats_(&stats), domain_(domain), inner_(inner) {}

  StateT summarize(const std::string &calleeName,
                   const StateT &inputAfterCall) override {
    if (stats_) {
      stats_->start(SifaStats::Key::CALL_SUMMARIZER_OVERALL_TIME);
      stats_->increment(SifaStats::Key::CALL_SUMMARIZER_APPLICATIONS);
    }
    SummaryCache<StateT> &cache = perCalleeCache_[calleeName];
    const auto isReusable = [this](const StateT &a, const StateT &b) {
      auto subsetEq = domain_.subsetEq(a, b);
      return subsetEq.isTrueForAbstraction() &&
             domain_.equal(subsetEq.getRhs(), b);
    };
    auto supersets = cache.reusableEntries(inputAfterCall, isReusable);
    StateT result;
    if (supersets.empty()) {
      result = inner_.summarize(calleeName, inputAfterCall);
      cache.store(inputAfterCall, result);
    } else if (supersets.size() == 1 || !domain_.supportsMeet()) {
      const auto exact =
          std::find_if(supersets.begin(), supersets.end(),
                       [this, &inputAfterCall](const auto &entry) {
                         return domain_.equal(entry.first, inputAfterCall);
                       });
      if (exact != supersets.end()) {
        result = exact->second;
      } else if (supersets.size() == 1) {
        result = supersets.front().second;
      } else {
        result = inner_.summarize(calleeName, inputAfterCall);
        cache.store(inputAfterCall, result);
      }
    } else {
      result = supersets.front().second;
      for (std::size_t i = 1; i < supersets.size(); ++i) {
        result = domain_.meet(result, supersets[i].second);
      }
    }
    if (stats_)
      stats_->stop(SifaStats::Key::CALL_SUMMARIZER_OVERALL_TIME);
    return result;
  }

private:
  SifaStats *stats_ = nullptr;
  const Domain &domain_;
  ICallSummarizer<StateT> &inner_;
  std::unordered_map<std::string, SummaryCache<StateT>> perCalleeCache_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_REUSESUPERSETCALLSUMMARIZER_H
