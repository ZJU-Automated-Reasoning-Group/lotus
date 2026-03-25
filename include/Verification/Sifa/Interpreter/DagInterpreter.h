//===-- Verification/Sifa/Interpreter/DagInterpreter.h --------------------===//
//
// DAG interpreter (ported from Ultimate Library-Sifa).
//
// Paper (TACAS 2020 "Ultimate Taipan with Symbolic Interpretation and Fluid
// Abstractions"): analyzes a RegexDAG in topological order, applying the post
// operator (domain_.post) for literals, loop summarization for Star, and call
// summarization for ReturnSummary transitions. Uses IFluid to decide when to
// apply abstraction (domain_.alpha). Multiple incoming edges -> join (∨).
//
// The DAG encodes a regex; interpretation propagates an abstract state from
// overlay sources to overlay sinks.
//
//===----------------------------------------------------------------------===//

#ifndef LOTUS_VERIFICATION_SIFA_INTERPRETER_DAGINTERPRETER_H
#define LOTUS_VERIFICATION_SIFA_INTERPRETER_DAGINTERPRETER_H

#include "llvm/IR/Function.h"

#include "Verification/Sifa/Caches/TopsortCache.h"
#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/Domain/AbstractDomain.h"
#include "Verification/Sifa/Fluid/IFluid.h"
#include "Verification/Sifa/Interpreter/IEnterCallRegistrar.h"
#include "Verification/Sifa/Interpreter/NoOpEnterCallRegistrar.h"
#include "Verification/Sifa/RegexDag/IDagOverlay.h"
#include "Verification/Sifa/Statistics/SifaStats.h"
#include "Verification/Sifa/Storage/MapBasedStorage.h"
#include "Verification/Sifa/Summarizers/ICallSummarizer.h"
#include "Verification/Sifa/Summarizers/ILoopSummarizer.h"
#include "Verification/Sifa/Worklist/PriorityWorklist.h"

#include <functional>
#include <stdexcept>
#include <type_traits>

namespace lotus {
namespace sifa {

template <typename L, typename StateT> class DagInterpreter final {
public:
  using Label = L;
  using State = StateT;
  using Dag = RegexDag<L>;
  using Node = RegexDagNode<L>;
  using Domain = AbstractDomain<L, State>;

  DagInterpreter(SifaStats &stats, const Domain &domain,
                 const IFluid<State> &fluid)
      : stats_(stats), domain_(domain), fluid_(fluid) {}

  void setLoopSummarizer(ILoopSummarizer<L, State> &loopSummarizer) {
    loopSummarizer_ = &loopSummarizer;
  }

  void setCallSummarizer(ICallSummarizer<State> *callSummarizer) {
    callSummarizer_ = callSummarizer;
  }

  void setEnterCallRegistrar(IEnterCallRegistrar<State> *enterCallRegistrar) {
    enterCallRegistrar_ = enterCallRegistrar;
  }

  /// Optional: set a timer that returns false when processing should stop
  /// (timeout). When set, checked each worklist iteration; interpretation stops
  /// and returns current state (over-approximation) if the timer returns false.
  void setTimer(std::function<bool()> continueProcessing) {
    timer_ = std::move(continueProcessing);
  }

  /// Interpret the DAG using only edges from the overlay and return the value
  /// at the only marker literal within that overlay (or bottom if unreachable).
  State interpretForSingleMarker(const Dag &dag, const IDagOverlay<L> &overlay,
                                 const State &initialInput) {
    MapBasedStorage<const llvm::BasicBlock *, State> storage;

    auto order = topsortCache_.topsort(dag);
    PriorityWorklist<Node *, State> worklist(
        order,
        [&](const State &a, const State &b) { return domain_.join(a, b); });

    for (Node *src : overlay.sources(dag)) {
      worklist.add(src, initialInput);
    }

    while (worklist.advance()) {
      if (timer_ && !timer_()) {
        break; // Timeout: return current over-approximation
      }
      Node *cur = worklist.getWork();
      State curIn = worklist.getInput();
      if (fluid_.shallBeAbstracted(curIn)) {
        curIn = domain_.alpha(curIn);
      }
      const State curOut = interpretNode(*cur, curIn, storage);

      if (earlyExitAfterStep(overlay, cur, curOut)) {
        continue;
      }
      for (Node *succ : overlay.successorsOf(cur)) {
        worklist.add(succ, curOut);
      }
    }

    return storage.getSingletonOrDefault(domain_.bottom());
  }

  /// Full interpretation: fill \p storage for all overlay sinks and call
  /// \p enterCallRegistrar when hitting EnterCall transitions. Used by
  /// IcfgInterpreter.
  void
  interpretWithCalls(const Dag &dag, const IDagOverlay<L> &overlay,
                     const State &initialInput,
                     MapBasedStorage<const llvm::BasicBlock *, State> &storage,
                     IEnterCallRegistrar<State> &enterCallRegistrar) {
    auto order = topsortCache_.topsort(dag);
    PriorityWorklist<Node *, State> worklist(
        order,
        [&](const State &a, const State &b) { return domain_.join(a, b); });

    for (Node *src : overlay.sources(dag)) {
      worklist.add(src, initialInput);
    }

    while (worklist.advance()) {
      if (timer_ && !timer_()) {
        break; // Timeout
      }
      Node *cur = worklist.getWork();
      State curIn = worklist.getInput();
      if (fluid_.shallBeAbstracted(curIn)) {
        curIn = domain_.alpha(curIn);
      }
      const State curOut =
          interpretNodeWithCalls(*cur, curIn, storage, enterCallRegistrar);

      if (earlyExitAfterStep(overlay, cur, curOut)) {
        continue;
      }
      for (Node *succ : overlay.successorsOf(cur)) {
        worklist.add(succ, curOut);
      }
    }
  }

private:
  /// Ultimate-aligned: isBottomLiteral first (cheap), then isEqBottom only at
  /// branches.
  bool earlyExitAfterStep(const IDagOverlay<L> &overlay, Node *curNode,
                          const State &curOut) {
    bool earlyExit = domain_.isBottomLiteral(curOut);
    if (!earlyExit && overlay.successorsOf(curNode).size() > 1) {
      stats_.increment(
          SifaStats::Key::DAG_INTERPRETER_EARLY_EXIT_QUERIES_NONTRIVIAL);
      earlyExit = domain_.isEqBottomResult(curOut).isTrueForAbstraction();
    }
    if (earlyExit) {
      stats_.increment(SifaStats::Key::DAG_INTERPRETER_EARLY_EXITS);
    }
    return earlyExit;
  }

  State
  interpretNode(const Node &node, const State &input,
                MapBasedStorage<const llvm::BasicBlock *, State> &storage) {
    return interpretNodeWithCalls(node, input, storage,
                                  *noOpEnterCallRegistrar());
  }

  State interpretNodeWithCalls(
      const Node &node, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage,
      IEnterCallRegistrar<State> &enterCallRegistrar) {
    const auto &regex = node.getContent();
    if (!regex) {
      throw std::logic_error("RegexDagNode has null content");
    }
    if (regex->isEpsilon()) {
      return input;
    }
    if (regex->isEmptySet()) {
      return domain_.bottom();
    }

    if (const auto *lit =
            dynamic_cast<const lotus::pathexpressions::Literal<L> *>(
                regex.get())) {
      return interpretTransitionWithCalls(lit->getLetter(), input, storage,
                                          enterCallRegistrar);
    }
    if (const auto *star =
            dynamic_cast<const lotus::pathexpressions::Star<L> *>(
                regex.get())) {
      stats_.increment(SifaStats::Key::LOOP_SUMMARIZER_APPLICATIONS);
      if (!loopSummarizer_) {
        throw std::logic_error("DagInterpreter missing loop summarizer");
      }
      return loopSummarizer_->summarize(*star, input);
    }
    throw std::logic_error("Unexpected node content in RegexDag");
  }

  static NoOpEnterCallRegistrar<State> *noOpEnterCallRegistrar() {
    static NoOpEnterCallRegistrar<State> noop;
    return &noop;
  }

  State interpretTransition(
      const L &t, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage) {
    return interpretTransitionWithCalls(t, input, storage,
                                        *noOpEnterCallRegistrar());
  }

  State interpretTransitionWithCalls(
      const L &t, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage,
      IEnterCallRegistrar<State> &enterCallRegistrar) {
    return interpretTransitionImplWithCalls(t, input, storage,
                                            enterCallRegistrar);
  }

  template <typename X>
  typename std::enable_if<std::is_same<X, Transition>::value, State>::type
  interpretTransitionImplWithCalls(
      const X &t, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage,
      IEnterCallRegistrar<State> &enterCallRegistrar) {
    if (t.kind == TransitionKind::Marker) {
      storage.store(t.target, input);
      return input;
    }
    if (t.kind == TransitionKind::EnterCall && t.callee) {
      const State stateAfterCall = domain_.postCall(t, input);
      enterCallRegistrar.registerEnterCall(t.callee->getName().str(),
                                           stateAfterCall);
      return stateAfterCall;
    }
    if (t.kind == TransitionKind::ReturnSummary && t.callee) {
      const State stateAfterCall = domain_.postCall(t, input);
      if (callSummarizer_) {
        const State summary = callSummarizer_->summarize(
            t.callee->getName().str(), stateAfterCall);
        return domain_.postReturn(t, input, summary);
      }
      // No summarizer (intraprocedural): optimistically propagate past the
      // call.
      return stateAfterCall;
    }
    return domain_.post(t, input);
  }

  template <typename X>
  typename std::enable_if<!std::is_same<X, Transition>::value, State>::type
  interpretTransitionImplWithCalls(
      const X &t, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage,
      IEnterCallRegistrar<State> &enterCallRegistrar) {
    (void)storage;
    (void)enterCallRegistrar;
    return domain_.post(t, input);
  }

  template <typename X>
  typename std::enable_if<std::is_same<X, Transition>::value, State>::type
  interpretTransitionImpl(
      const X &t, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage) {
    if (t.kind == TransitionKind::Marker) {
      storage.store(t.target, input);
      return input;
    }
    return domain_.post(t, input);
  }

  template <typename X>
  typename std::enable_if<!std::is_same<X, Transition>::value, State>::type
  interpretTransitionImpl(
      const X &t, const State &input,
      MapBasedStorage<const llvm::BasicBlock *, State> &storage) {
    (void)storage;
    return domain_.post(t, input);
  }

  SifaStats &stats_;
  const Domain &domain_;
  const IFluid<State> &fluid_;
  ILoopSummarizer<L, State> *loopSummarizer_ = nullptr;
  ICallSummarizer<State> *callSummarizer_ = nullptr;
  IEnterCallRegistrar<State> *enterCallRegistrar_ = nullptr;
  std::function<bool()> timer_; // Optional: return false to stop (timeout)
  TopsortCache<L> topsortCache_;
};

} // namespace sifa
} // namespace lotus

#include "Verification/Sifa/Cfg/Transition.h"
#include "Verification/Sifa/SifaSymAbs.h"
extern template class lotus::sifa::DagInterpreter<lotus::sifa::Transition,
                                                  bool>;
extern template class lotus::sifa::DagInterpreter<lotus::sifa::Transition,
                                                  lotus::sifa::SymAbsState>;

#endif // LOTUS_VERIFICATION_SIFA_INTERPRETER_DAGINTERPRETER_H
