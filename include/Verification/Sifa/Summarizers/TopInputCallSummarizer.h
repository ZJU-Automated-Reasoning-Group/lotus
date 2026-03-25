//===-- Verification/Sifa/Summarizers/TopInputCallSummarizer.h ------------===//
//
// Call summarizer that interprets callee with top input and caches
// (Ultimate-aligned).
//
// Ultimate's TopInputCallSummarizer: per-callee cache; summarize(callee, input)
// returns cached summary or interpretForSingleMarker(dag, pathToReturn,
// tools.top()).
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_TOPINPUTCALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_TOPINPUTCALLSUMMARIZER_H

#include "llvm/ADT/Optional.h"

#include "Verification/Sifa/Caches/ProcedureResourceCache.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"

#include <string>
#include <unordered_map>

namespace lotus {
namespace sifa {

template <typename StateT>
class TopInputCallSummarizer final : public ICallSummarizer<StateT> {
public:
  using Domain = AbstractDomain<Transition, StateT>;

  /// Stub: always returns the given top state (e.g. for tests or
  /// intraprocedural only).
  explicit TopInputCallSummarizer(StateT topState)
      : useStub_(true), topState_(std::move(topState)) {}

  /// Ultimate-aligned: interpret callee with domain.top(), cache per callee.
  /// Requires ProcedureResourceCache built with Module and CallGraph for
  /// resourcesOf(name).
  TopInputCallSummarizer(SifaStats &stats, const Domain &domain,
                         ProcedureResourceCache &cache,
                         DagInterpreter<Transition, StateT> &dagIpr)
      : useStub_(false), stats_(&stats), domain_(&domain), cache_(&cache),
        dagIpr_(&dagIpr) {}

  StateT summarize(const std::string &calleeName,
                   const StateT &inputAfterCall) override {
    if (useStub_) {
      (void)calleeName;
      (void)inputAfterCall;
      return *topState_;
    }
    stats_->start(SifaStats::Key::CALL_SUMMARIZER_OVERALL_TIME);
    stats_->increment(SifaStats::Key::CALL_SUMMARIZER_APPLICATIONS);

    StateT result;
    auto it = procToSummary_.find(calleeName);
    if (it != procToSummary_.end()) {
      result = it->second;
    } else {
      result = computeTopSummary(calleeName);
      procToSummary_.emplace(calleeName, result);
    }

    stats_->stop(SifaStats::Key::CALL_SUMMARIZER_OVERALL_TIME);
    return result;
  }

private:
  StateT computeTopSummary(const std::string &callee) {
    stats_->start(SifaStats::Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME);
    stats_->increment(SifaStats::Key::CALL_SUMMARIZER_CACHE_MISSES);

    const ProcedureResources &res = cache_->resourcesOf(callee);
    StateT result = dagIpr_->interpretForSingleMarker(
        res.getRegexDag(), res.getDagOverlayPathToReturn(), domain_->top());

    stats_->stop(SifaStats::Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME);
    return result;
  }

  bool useStub_ = true;
  llvm::Optional<StateT> topState_; // stub only

  SifaStats *stats_ = nullptr;
  const Domain *domain_ = nullptr;
  ProcedureResourceCache *cache_ = nullptr;
  DagInterpreter<Transition, StateT> *dagIpr_ = nullptr;
  std::unordered_map<std::string, StateT> procToSummary_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_TOPINPUTCALLSUMMARIZER_H
