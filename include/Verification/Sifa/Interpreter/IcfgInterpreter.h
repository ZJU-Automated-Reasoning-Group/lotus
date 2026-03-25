//===-- Verification/Sifa/Interpreter/IcfgInterpreter.h ------------------===//
//
// ICFG interpreter (ported from Ultimate Library-Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan with Symbolic Interpretation and Fluid
// Abstractions"): generates for a (partial) ICFG and LOIs (locations of
// interest) a set of path expressions as RegexDAGs. Coordinates DagInterpreter
// per procedure, enter-call registration, and call summarization.
//
// Interprets from entry procedures, fills storage for LOIs, and uses
// IEnterCallRegistrar + ICallSummarizer for call/return handling.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_INTERPRETER_ICFGINTERPRETER_H
#define LOTUS_VERIFICATION_SIFA_INTERPRETER_ICFGINTERPRETER_H

#include "llvm/IR/Module.h"

#include "Verification/Sifa/Caches/ProcedureResourceCache.h"
#include "Verification/Sifa/CallGraph.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Fluid/IFluid.h"
#include "Verification/Sifa/Interpreter/DagInterpreter.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/FixpointLoopSummarizer.h"
#include "Verification/Sifa/Summarizers/InterpretCallSummarizer.h"
#include "Verification/Sifa/Worklist/PriorityWorklist.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <vector>

namespace lotus {
namespace sifa {

/// Interprocedural Sifa interpreter: worklist of (procedure, input state),
/// DagInterpreter per procedure, enter-call registration and call
/// summarization. Ultimate-aligned: optional logging hooks (logStartingSifa,
/// logEnterProcedure, etc.).
template <typename StateT> class IcfgInterpreter {
public:
  using LOI = CallGraph::LOI;
  using Domain = AbstractDomain<Transition, StateT>;
  using DagIpr = DagInterpreter<Transition, StateT>;
  using LoopSummarizer = ILoopSummarizer<Transition, StateT>;
  using CallSummarizer = ICallSummarizer<StateT>;
  using LoopSummarizerFactory =
      std::function<std::unique_ptr<LoopSummarizer>(DagIpr &)>;
  using CallSummarizerFactory =
      std::function<std::unique_ptr<CallSummarizer>(DagIpr &)>;

  /// Ultimate-aligned: allErrorLocations(icfg). Returns LOIs for all blocks
  /// that call error-like functions (e.g. __VERIFIER_error, abort). Use as LOI
  /// set for interpret() when targeting error locations.
  static std::vector<LOI> allErrorLocations(const llvm::Module &M) {
    return CallGraph::gatherErrorLocations(M);
  }

  IcfgInterpreter(const llvm::Module &M, const llvm::Function *entry,
                  const std::vector<LOI> &locationsOfInterest, SifaStats &stats,
                  const Domain &domain, const IFluid<StateT> &fluid,
                  const StateT &initialState)
      : IcfgInterpreter(M,
                        entry ? llvm::ArrayRef<const llvm::Function *>{entry}
                              : llvm::ArrayRef<const llvm::Function *>{},
                        locationsOfInterest, stats, domain, fluid,
                        initialState) {}

  IcfgInterpreter(const llvm::Module &M,
                  llvm::ArrayRef<const llvm::Function *> initialProcedures,
                  const std::vector<LOI> &locationsOfInterest, SifaStats &stats,
                  const Domain &domain, const IFluid<StateT> &fluid,
                  const StateT &initialState)
      : M_(M), lois_(locationsOfInterest), stats_(stats), domain_(domain),
        fluid_(fluid), initialState_(initialState),
        cg_(M, initialProcedures, locationsOfInterest),
        procResCache_(stats_, cg_, M) {
    resetSummarizerFactories();
  }

  /// Ultimate-aligned: optional logging. No-op if not set.
  void setOnStartingInterpretation(std::function<void()> f) {
    onStartingInterpretation_ = std::move(f);
  }
  void setOnEnterProcedure(
      std::function<void(const llvm::Function *, const StateT &)> f) {
    onEnterProcedure_ = std::move(f);
  }
  void setOnInterpretationFinished(
      std::function<
          void(const MapBasedStorage<const llvm::BasicBlock *, StateT> &)>
          f) {
    onInterpretationFinished_ = std::move(f);
  }
  void setLoopSummarizerFactory(LoopSummarizerFactory factory) {
    loopSummarizerFactory_ = std::move(factory);
  }
  void setCallSummarizerFactory(CallSummarizerFactory factory) {
    callSummarizerFactory_ = std::move(factory);
  }

  /// Ultimate-aligned: callGraph(), procedureResourceCache().
  const CallGraph &callGraph() const { return cg_; }
  const ProcedureResourceCache &procedureResourceCache() const {
    return procResCache_;
  }

  /// Run interprocedural interpretation; fill \p storage with states at LOIs.
  /// Ultimate-aligned: OVERALL_TIME start/stop,
  /// ICFG_INTERPRETER_ENTERED_PROCEDURES.
  void interpret(MapBasedStorage<const llvm::BasicBlock *, StateT> &storage) {
    stats_.start(SifaStats::Key::OVERALL_TIME);
    if (onStartingInterpretation_)
      onStartingInterpretation_();

    DagIpr dagInterpreter(stats_, domain_, fluid_);
    std::unique_ptr<LoopSummarizer> loopSum =
        loopSummarizerFactory_ ? loopSummarizerFactory_(dagInterpreter)
                               : nullptr;
    if (!loopSum) {
      throw std::logic_error("IcfgInterpreter missing loop summarizer");
    }
    dagInterpreter.setLoopSummarizer(*loopSum);

    std::unique_ptr<CallSummarizer> callSum =
        callSummarizerFactory_ ? callSummarizerFactory_(dagInterpreter)
                               : nullptr;
    if (callSum) {
      dagInterpreter.setCallSummarizer(callSum.get());
    }

    const auto &procs = cg_.relevantProceduresTopsorted();
    std::vector<const llvm::Function *> order(procs.begin(), procs.end());
    PriorityWorklist<const llvm::Function *, StateT> worklist(
        order,
        [&](const StateT &a, const StateT &b) { return domain_.join(a, b); });
    IcfgEnterCallRegistrar enterCallRegistrar(&M_, &worklist);
    dagInterpreter.setEnterCallRegistrar(&enterCallRegistrar);

    for (const llvm::Function *proc : cg_.initialProceduresOfInterest()) {
      worklist.add(proc, initialState_);
    }

    while (worklist.advance()) {
      const llvm::Function *F = worklist.getWork();
      const StateT input = worklist.getInput();

      stats_.increment(SifaStats::Key::ICFG_INTERPRETER_ENTERED_PROCEDURES);
      if (onEnterProcedure_)
        onEnterProcedure_(F, input);

      const ProcedureResources &res = procResCache_.resourcesOf(*F);
      dagInterpreter.interpretWithCalls(
          res.getRegexDag(), res.getDagOverlayPathToLoisAndEnterCalls(), input,
          storage, enterCallRegistrar);
    }

    std::vector<const llvm::BasicBlock *> loiBlocks;
    loiBlocks.reserve(lois_.size());
    for (const LOI &loi : lois_) {
      if (loi.second) {
        loiBlocks.push_back(loi.second);
      }
    }
    storage.addDefaultsAndGetMap(loiBlocks, domain_.bottom());

    stats_.stop(SifaStats::Key::OVERALL_TIME);
    if (onInterpretationFinished_)
      onInterpretationFinished_(storage);
  }

private:
  void resetSummarizerFactories() {
    loopSummarizerFactory_ = [this](DagIpr &dagInterpreter) {
      return std::unique_ptr<LoopSummarizer>(
          new FixpointLoopSummarizer<Transition, StateT>(
              stats_, domain_, fluid_, dagInterpreter));
    };
    callSummarizerFactory_ = [this](DagIpr &dagInterpreter) {
      return std::unique_ptr<CallSummarizer>(
          new InterpretCallSummarizer<StateT>(stats_, M_, procResCache_,
                                              dagInterpreter, domain_));
    };
  }

  std::function<void()> onStartingInterpretation_;
  std::function<void(const llvm::Function *, const StateT &)> onEnterProcedure_;
  std::function<void(const MapBasedStorage<const llvm::BasicBlock *, StateT> &)>
      onInterpretationFinished_;
  /// Worklist registrar: adds (callee, state) to the procedure worklist.
  class IcfgEnterCallRegistrar final : public IEnterCallRegistrar<StateT> {
  public:
    IcfgEnterCallRegistrar(const llvm::Module *M,
                           PriorityWorklist<const llvm::Function *, StateT> *wl)
        : M_(M), worklist_(wl) {}

    void registerEnterCall(const std::string &calleeName,
                           const StateT &calleeInput) override {
      if (!worklist_ || !M_)
        return;
      const llvm::Function *callee = M_->getFunction(calleeName);
      if (callee && !callee->isDeclaration())
        worklist_->add(callee, calleeInput);
    }

  private:
    const llvm::Module *M_ = nullptr;
    PriorityWorklist<const llvm::Function *, StateT> *worklist_ = nullptr;
  };

  const llvm::Module &M_;
  std::vector<LOI> lois_;
  SifaStats &stats_;
  const Domain &domain_;
  const IFluid<StateT> &fluid_;
  StateT initialState_;
  LoopSummarizerFactory loopSummarizerFactory_;
  CallSummarizerFactory callSummarizerFactory_;
  CallGraph cg_;
  ProcedureResourceCache procResCache_;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Cfg/Transition.h"
extern template class lotus::sifa::IcfgInterpreter<bool>;

#endif // LOTUS_VERIFICATION_SIFA_INTERPRETER_ICFGINTERPRETER_H
