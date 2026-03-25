//===-- Verification/Sifa/Summarizers/InterpretCallSummarizer.h -----------===//
//
// Call summarization operator (ported from Ultimate Library-Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan..."): the call summarization operator
// computes a summary for a procedure call, either with or without considering
// the context. This implementation interprets the callee (entry->return) to
// compute the summary.
//
// Ultimate-aligned: SifaStats CALL_SUMMARIZER_NEW_COMPUTATION_TIME,
// CACHE_MISSES.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_SUMMARIZERS_INTERPRETCALLSUMMARIZER_H
#define LOTUS_VERIFICATION_SIFA_SUMMARIZERS_INTERPRETCALLSUMMARIZER_H

#include "llvm/IR/Module.h"

#include "Verification/Sifa/Caches/ProcedureResourceCache.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Procedure/ProcedureResources.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace lotus {
namespace sifa {

template <typename StateT>
class InterpretCallSummarizer final : public ICallSummarizer<StateT> {
public:
  using Domain = AbstractDomain<Transition, StateT>;

  InterpretCallSummarizer(SifaStats &stats, const llvm::Module &M,
                          ProcedureResourceCache &cache,
                          DagInterpreter<Transition, StateT> &dagInterpreter,
                          const Domain &domain)
      : stats_(stats), M_(M), cache_(cache), dagInterpreter_(dagInterpreter),
        domain_(&domain) {}

  /// Legacy constructor without domain (uses default equal() via leq).
  InterpretCallSummarizer(SifaStats &stats, const llvm::Module &M,
                          ProcedureResourceCache &cache,
                          DagInterpreter<Transition, StateT> &dagInterpreter)
      : stats_(stats), M_(M), cache_(cache), dagInterpreter_(dagInterpreter),
        domain_(nullptr) {}

  StateT summarize(const std::string &calleeName,
                   const StateT &inputAfterCall) override {
    const llvm::Function *callee = M_.getFunction(calleeName);
    if (!callee || callee->isDeclaration()) {
      // External function: assume identity (conservative).
      return inputAfterCall;
    }

    stats_.increment(SifaStats::Key::CALL_SUMMARIZER_APPLICATIONS);

    // Check result cache: avoid re-interpreting the same callee with the same
    // input state. Cache is keyed by (callee pointer, input state index).
    auto &entries = resultCache_[callee];
    for (auto &e : entries) {
      if (statesEqual(e.first, inputAfterCall)) {
        return e.second;
      }
    }

    stats_.increment(SifaStats::Key::CALL_SUMMARIZER_CACHE_MISSES);
    stats_.start(SifaStats::Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME);

    const ProcedureResources &res = cache_.resourcesOf(*callee);
    StateT result = dagInterpreter_.interpretForSingleMarker(
        res.getRegexDag(), res.getDagOverlayPathToReturn(), inputAfterCall);

    stats_.stop(SifaStats::Key::CALL_SUMMARIZER_NEW_COMPUTATION_TIME);

    // Store in cache.
    entries.emplace_back(inputAfterCall, result);
    return result;
  }

private:
  bool statesEqual(const StateT &a, const StateT &b) const {
    if (domain_)
      return domain_->equal(a, b);
    // Fallback: use operator== if available (works for bool, etc.).
    return a == b;
  }

  SifaStats &stats_;
  const llvm::Module &M_;
  ProcedureResourceCache &cache_;
  DagInterpreter<Transition, StateT> &dagInterpreter_;
  const Domain *domain_ = nullptr;

  // Per-callee result cache: vector of (input, result) pairs.
  // Linear scan is acceptable because the number of distinct inputs per callee
  // is typically small (bounded by the number of call sites).
  std::unordered_map<const llvm::Function *,
                     std::vector<std::pair<StateT, StateT>>>
      resultCache_;
};

} // namespace sifa
} // namespace lotus

#endif // LOTUS_VERIFICATION_SIFA_SUMMARIZERS_INTERPRETCALLSUMMARIZER_H
