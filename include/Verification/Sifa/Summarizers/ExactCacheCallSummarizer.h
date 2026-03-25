//===-- Verification/Sifa/Summarizers/ExactCacheCallSummarizer.h ----------===//
//
// ICallSummarizer wrapper that caches (callee, input) -> result by exact match.
//
// Use when you need an ICallSummarizer that avoids recomputing the same
// (calleeName, inputAfterCall) summary. Ultimate uses SummaryCache only for
// reUseOrCompute (subset-based re-use); this class provides exact-match
// caching.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_EXACTCACHECALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_EXACTCACHECALLSUMMARIZER_H

#include "Verification/Sifa/Summarizers/ICallSummarizer.h"

#include <string>
#include <tuple>
#include <vector>

namespace lotus {
namespace sifa {

template <typename StateT>
class ExactCacheCallSummarizer final : public ICallSummarizer<StateT> {
public:
  explicit ExactCacheCallSummarizer(ICallSummarizer<StateT> &inner)
      : inner_(inner) {}

  StateT summarize(const std::string &calleeName,
                   const StateT &inputAfterCall) override {
    for (const auto &e : cache_) {
      if (std::get<0>(e) == calleeName && std::get<1>(e) == inputAfterCall)
        return std::get<2>(e);
    }
    StateT result = inner_.summarize(calleeName, inputAfterCall);
    cache_.emplace_back(calleeName, inputAfterCall, result);
    return result;
  }

private:
  ICallSummarizer<StateT> &inner_;
  std::vector<std::tuple<std::string, StateT, StateT>> cache_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_EXACTCACHECALLSUMMARIZER_H
