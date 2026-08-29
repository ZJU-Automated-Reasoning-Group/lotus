#ifndef NPA_LLVM_BACKWARD_INTER_ENGINE_H
#define NPA_LLVM_BACKWARD_INTER_ENGINE_H

#include "Dataflow/NPA/LLVM/ForwardInterEngine.h"
#include "Dataflow/NPA/NPA.h"
#include "Utils/Algorithms/PathExpressions/PathExpressions.h"
#include "Utils/Parallel/ThreadPool.h"

#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace npa {

template <class D, class Analysis> class BackwardInterEngine {
public:
  using Exp = Exp0<D>;
  using E = E0<D>;
  using Val = typename D::value_type;
  using Fact = typename Analysis::FactType;

  struct Result {
    AnalysisStatus status;
    std::map<FunctionKey, Val> summaries;
    std::map<BlockKey, Fact> blockEntryFacts;
  };

private:
  struct ApproximationFlags {
    // V1 contract: one analysis run at a time. The engine owns these
    // run-local flags so clients can keep propagation hooks side-effect free
    // after construction without adding synchronization to hot paths.
    bool used_summary_overflow = false;
    bool used_fact_widening = false;
  };

  template <typename A>
  static auto getMaxPropagationSteps(const A &analysis, int)
      -> decltype(analysis.getMaxPropagationSteps()) {
    return analysis.getMaxPropagationSteps();
  }

  static long getMaxPropagationSteps(const Analysis &, long) { return -1; }

  template <typename A>
  static auto widenFacts(A &analysis, const Fact &oldFact, const Fact &newFact,
                         size_t updates, int)
      -> decltype(analysis.widenFacts(oldFact, newFact, updates)) {
    return analysis.widenFacts(oldFact, newFact, updates);
  }

  static Fact widenFacts(Analysis &, const Fact &, const Fact &newFact, size_t,
                         long) {
    return newFact;
  }

  template <typename A>
  static auto hasCustomWidenFacts(const A &, int)
      -> decltype(std::declval<A &>().widenFacts(std::declval<const Fact &>(),
                                                 std::declval<const Fact &>(),
                                                 std::size_t{}),
                  bool()) {
    return true;
  }

  static bool hasCustomWidenFacts(const Analysis &, long) { return false; }

  template <typename A>
  static auto summaryIsApproximate(const A &analysis, const Val &summary, int)
      -> decltype(analysis.summaryIsApproximate(summary)) {
    return analysis.summaryIsApproximate(summary);
  }

  static bool summaryIsApproximate(const Analysis &, const Val &, long) {
    return false;
  }

  template <typename A>
  static auto applySummaryWithReporting(A &analysis, const Val &summary,
                                        const Fact &fact,
                                        ApproximationFlags &flags, int)
      -> decltype(analysis.applySummary(summary, fact,
                                        &flags.used_summary_overflow)) {
    return analysis.applySummary(summary, fact, &flags.used_summary_overflow);
  }

  static Fact applySummaryWithReporting(Analysis &analysis, const Val &summary,
                                        const Fact &fact, ApproximationFlags &,
                                        long) {
    return analysis.applySummary(summary, fact);
  }

  template <typename A>
  static auto factIsApproximate(const A &analysis, const Fact &fact, int)
      -> decltype(analysis.factIsApproximate(fact)) {
    return analysis.factIsApproximate(fact);
  }

  static bool factIsApproximate(const Analysis &, const Fact &, long) {
    return false;
  }

  template <typename A>
  static auto widenFactsWithReporting(A &analysis, const Fact &oldFact,
                                      const Fact &newFact, size_t updates,
                                      ApproximationFlags &flags, int)
      -> decltype(analysis.widenFacts(oldFact, newFact, updates,
                                      &flags.used_fact_widening)) {
    return analysis.widenFacts(oldFact, newFact, updates,
                               &flags.used_fact_widening);
  }

  template <typename A>
  static auto widenFactsWithReporting(A &analysis, const Fact &oldFact,
                                      const Fact &newFact, size_t updates,
                                      ApproximationFlags &, long)
      -> decltype(analysis.widenFacts(oldFact, newFact, updates)) {
    return analysis.widenFacts(oldFact, newFact, updates);
  }

  static Fact widenFactsWithReporting(Analysis &, const Fact &,
                                      const Fact &newFact, size_t,
                                      ApproximationFlags &, ...) {
    return newFact;
  }

  template <typename A>
  static auto getCallEntryTransfer(A &analysis, const llvm::CallBase &call,
                                   const llvm::Function &callee, int)
      -> decltype(analysis.getCallEntryTransfer(call, callee)) {
    return analysis.getCallEntryTransfer(call, callee);
  }

  static typename D::value_type getCallEntryTransfer(Analysis &,
                                                     const llvm::CallBase &,
                                                     const llvm::Function &,
                                                     long) {
    return D::one();
  }

  template <typename A>
  static auto getCallReturnTransfer(A &analysis, const llvm::CallBase &call,
                                    const llvm::Function &callee, int)
      -> decltype(analysis.getCallReturnTransfer(call, callee)) {
    return analysis.getCallReturnTransfer(call, callee);
  }

  static typename D::value_type getCallReturnTransfer(Analysis &,
                                                      const llvm::CallBase &,
                                                      const llvm::Function &,
                                                      long) {
    return D::one();
  }

  template <typename A>
  static auto getCallToReturnTransfer(A &analysis, const llvm::CallBase &call,
                                      int)
      -> decltype(analysis.getCallToReturnTransfer(call)) {
    return analysis.getCallToReturnTransfer(call);
  }

  static typename D::value_type
  getCallToReturnTransfer(Analysis &, const llvm::CallBase &, long) {
    return D::one();
  }

  template <typename A>
  static auto getEdgeTransfer(A &analysis, const llvm::Instruction &term,
                              const llvm::BasicBlock &succ, int)
      -> decltype(analysis.getEdgeTransfer(term, succ)) {
    return analysis.getEdgeTransfer(term, succ);
  }

  static typename D::value_type getEdgeTransfer(Analysis &,
                                                const llvm::Instruction &,
                                                const llvm::BasicBlock &,
                                                long) {
    return D::one();
  }

  struct FunctionRegexArtifacts {
    E summaryExpr;
    E fullSummaryExpr;
    std::unordered_map<std::string, E> blockSummaryExprs;
  };

  struct PreparedFunctionArtifacts {
    llvm::Function *function = nullptr;
    FunctionRegexArtifacts artifacts;
    AnalysisStatus status_delta;
    std::vector<llvm::Function *> discovered_callees;
  };

  static bool isZeroExpr(const E &expr) {
    return expr && expr->k == Exp::Term && D::equal(expr->c, D::zero());
  }

  static bool isOneExpr(const E &expr) {
    return expr && expr->k == Exp::Term && D::equal(expr->c, D::one());
  }

  static E combineExpr(E lhs, E rhs) {
    if (!lhs)
      return rhs;
    if (!rhs)
      return lhs;
    if (isZeroExpr(lhs))
      return rhs;
    if (isZeroExpr(rhs))
      return lhs;
    return Exp::ndet(lhs, rhs);
  }

  static E multiplyExpr(E lhs, E rhs) {
    if (!lhs || !rhs)
      return Exp::term(D::zero());
    if (isZeroExpr(lhs) || isZeroExpr(rhs))
      return Exp::term(D::zero());
    if (isOneExpr(lhs))
      return rhs;
    if (isOneExpr(rhs))
      return lhs;
    if (lhs->k == Exp::Term && rhs->k == Exp::Term)
      return Exp::term(D::extend(lhs->c, rhs->c));
    if (lhs->k == Exp::Term)
      return Exp::seq(lhs->c, rhs);
    return Exp::mul(lhs, rhs);
  }

  static E starExpr(E inner, unsigned &counter) {
    if (!inner || isZeroExpr(inner) || isOneExpr(inner))
      return Exp::term(D::one());
    Symbol bound = "__bwd_regex_star_" + std::to_string(counter++);
    return Exp::star(
        combineExpr(Exp::term(D::one()), Exp::mul(Exp::bound(bound), inner)),
        bound);
  }

  class RegexToExpr final
      : public lotus::pathexpressions::IRegexVisitor<int, E, std::nullptr_t> {
  public:
    RegexToExpr(const std::vector<E> &labels, unsigned &star_counter)
        : labels_(labels), starCounter_(star_counter) {}

    E visit(const lotus::pathexpressions::Union<int> &re,
            std::nullptr_t) override {
      return combineExpr(re.getFirst()->accept(*this),
                         re.getSecond()->accept(*this));
    }

    E visit(const lotus::pathexpressions::Concatenation<int> &re,
            std::nullptr_t) override {
      return multiplyExpr(re.getSecond()->accept(*this),
                          re.getFirst()->accept(*this));
    }

    E visit(const lotus::pathexpressions::Star<int> &re,
            std::nullptr_t) override {
      return starExpr(re.getInner()->accept(*this), starCounter_);
    }

    E visit(const lotus::pathexpressions::Literal<int> &re,
            std::nullptr_t) override {
      return labels_.at(static_cast<std::size_t>(re.getLetter()));
    }

    E visit(const lotus::pathexpressions::Epsilon<int> &,
            std::nullptr_t) override {
      return Exp::term(D::one());
    }

    E visit(const lotus::pathexpressions::EmptySet<int> &,
            std::nullptr_t) override {
      return Exp::term(D::zero());
    }

  private:
    const std::vector<E> &labels_;
    unsigned &starCounter_;
  };

  static E translateRegex(const lotus::pathexpressions::RegexRef<int> &regex,
                          const std::vector<E> &labels,
                          unsigned &star_counter) {
    RegexToExpr translator(labels, star_counter);
    return regex->accept(translator, nullptr);
  }

  template <class T = D>
  static typename std::enable_if<DomainHasProject<T>::value, E>::type
  makeSummaryEquationExpr(E expr) {
    return Exp::project(std::move(expr));
  }

  template <class T = D>
  static typename std::enable_if<!DomainHasProject<T>::value, E>::type
  makeSummaryEquationExpr(E expr) {
    return expr;
  }

  static E makeCallSummaryExpr(Symbol sym) { return Exp::hole(std::move(sym)); }

  static E buildBlockBodyExpr(
      Analysis &analysis, llvm::Instruction &I, E currentPath,
      typename InterEngine<D, Analysis>::CalleeCache &calleeCache,
      std::deque<llvm::Function *> &worklist,
      std::set<llvm::Function *> &visited, AnalysisStatus &status,
      IndirectCallResolutionMode callResolutionMode) {
    if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
      if (CI->getCalledFunction() == nullptr)
        ++status.indirect_calls_seen;
      std::vector<llvm::Function *> Callees = calleeCache.get(*CI);
      if (!Callees.empty()) {
        E callBranches = nullptr;
        for (llvm::Function *Callee : Callees) {
          const Symbol callee_sym =
              InterEngine<D, Analysis>::getFuncSymbol(Callee);
          if (visited.insert(Callee).second)
            worklist.push_back(Callee);
          E branch = Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              currentPath);
          branch = multiplyExpr(makeCallSummaryExpr(callee_sym), branch);
          branch =
              Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0), branch);
          callBranches = combineExpr(callBranches, branch);
        }
        currentPath = callBranches;
      } else {
        if (CI->getCalledFunction() == nullptr) {
          ++status.unresolved_indirect_calls;
          if (callResolutionMode ==
              IndirectCallResolutionMode::CustomResolverRequired) {
            status.requires_external_callee_resolver = true;
            status.approximated = true;
          }
        }
        currentPath =
            Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
      }
    }
    return analysis.getTransfer(I, currentPath);
  }

  static E buildBlockBodyExprPrepared(
      Analysis &analysis, llvm::Instruction &I, E currentPath,
      typename InterEngine<D, Analysis>::CalleeCache &calleeCache,
      std::vector<llvm::Function *> &discovered, AnalysisStatus &status,
      IndirectCallResolutionMode callResolutionMode) {
    if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
      if (CI->getCalledFunction() == nullptr)
        ++status.indirect_calls_seen;
      std::vector<llvm::Function *> Callees = calleeCache.get(*CI);
      if (!Callees.empty()) {
        E callBranches = nullptr;
        for (llvm::Function *Callee : Callees) {
          if (!Callee->isDeclaration())
            discovered.push_back(Callee);
          const Symbol callee_sym =
              InterEngine<D, Analysis>::getFuncSymbol(Callee);
          E branch = Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              currentPath);
          branch = multiplyExpr(makeCallSummaryExpr(callee_sym), branch);
          branch =
              Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0), branch);
          callBranches = combineExpr(callBranches, branch);
        }
        currentPath = callBranches;
      } else {
        if (CI->getCalledFunction() == nullptr) {
          ++status.unresolved_indirect_calls;
          if (callResolutionMode ==
              IndirectCallResolutionMode::CustomResolverRequired) {
            status.requires_external_callee_resolver = true;
            status.approximated = true;
          }
        }
        ++status.fallback_call_edges;
        currentPath =
            Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
      }
    }
    return analysis.getTransfer(I, currentPath);
  }

  static FunctionRegexArtifacts buildFunctionRegexArtifacts(
      llvm::Module &M, llvm::Function &F, Analysis &analysis,
      typename InterEngine<D, Analysis>::CalleeCache &calleeCache,
      std::deque<llvm::Function *> &worklist,
      std::set<llvm::Function *> &visited, AnalysisStatus &status,
      IndirectCallResolutionMode callResolutionMode) {
    using Graph = lotus::pathexpressions::GenericLabeledGraph<int, int>;

    std::unordered_map<const llvm::BasicBlock *, int> blockIds;
    int nextId = 0;
    for (auto &BB : F)
      blockIds[&BB] = nextId++;
    const int exitId = nextId;

    Graph graph;
    graph.addNode(exitId);

    std::vector<E> labels;
    labels.reserve(F.size() * 2U + 1U);
    auto addLabel = [&](E expr) {
      labels.push_back(expr ? expr : Exp::term(D::zero()));
      return static_cast<int>(labels.size() - 1);
    };

    for (auto &BB : F) {
      const int fromId = blockIds.at(&BB);
      graph.addNode(fromId);

      E currentPath = Exp::term(D::one());
      for (auto It = BB.rbegin(); It != BB.rend(); ++It)
        currentPath =
            buildBlockBodyExpr(analysis, *It, currentPath, calleeCache,
                               worklist, visited, status, callResolutionMode);

      auto *Term = BB.getTerminator();
      if (!Term || Term->getNumSuccessors() == 0) {
        graph.addEdge(fromId, addLabel(currentPath), exitId);
        continue;
      }

      for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
        auto *Succ = Term->getSuccessor(i);
        if (!Succ)
          continue;
        E edgeExpr =
            Exp::seq(getEdgeTransfer(analysis, *Term, *Succ, 0), currentPath);
        graph.addEdge(fromId, addLabel(edgeExpr), blockIds.at(Succ));
      }
    }

    lotus::pathexpressions::PathExpressionComputer<int, int> computer(graph);
    unsigned starCounter = 0;

    FunctionRegexArtifacts out;
    llvm::BasicBlock *Entry = F.empty() ? nullptr : &F.getEntryBlock();
    out.fullSummaryExpr =
        Entry ? translateRegex(computer.exprBetween(blockIds.at(Entry), exitId),
                               labels, starCounter)
              : Exp::term(D::one());
    out.summaryExpr = makeSummaryEquationExpr(out.fullSummaryExpr);
    for (auto &BB : F) {
      const std::string bSym = InterEngine<D, Analysis>::getBlockSymbol(&BB);
      out.blockSummaryExprs.emplace(
          bSym, translateRegex(computer.exprBetween(blockIds.at(&BB), exitId),
                               labels, starCounter));
    }
    return out;
  }

  static PreparedFunctionArtifacts prepareFunctionRegexArtifacts(
      llvm::Module &M, llvm::Function &F, Analysis &analysis,
      typename InterEngine<D, Analysis>::CalleeCache &calleeCache,
      IndirectCallResolutionMode callResolutionMode) {
    using Graph = lotus::pathexpressions::GenericLabeledGraph<int, int>;

    std::unordered_map<const llvm::BasicBlock *, int> blockIds;
    int nextId = 0;
    for (auto &BB : F)
      blockIds[&BB] = nextId++;
    const int exitId = nextId;

    Graph graph;
    graph.addNode(exitId);

    std::vector<E> labels;
    labels.reserve(F.size() * 2U + 1U);
    auto addLabel = [&](E expr) {
      labels.push_back(expr ? expr : Exp::term(D::zero()));
      return static_cast<int>(labels.size() - 1);
    };

    PreparedFunctionArtifacts prepared;
    prepared.function = &F;

    for (auto &BB : F) {
      const int fromId = blockIds.at(&BB);
      graph.addNode(fromId);

      E currentPath = Exp::term(D::one());
      for (auto It = BB.rbegin(); It != BB.rend(); ++It)
        currentPath = buildBlockBodyExprPrepared(
            analysis, *It, currentPath, calleeCache,
            prepared.discovered_callees, prepared.status_delta,
            callResolutionMode);

      auto *Term = BB.getTerminator();
      if (!Term || Term->getNumSuccessors() == 0) {
        graph.addEdge(fromId, addLabel(currentPath), exitId);
        continue;
      }

      for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
        auto *Succ = Term->getSuccessor(i);
        if (!Succ)
          continue;
        E edgeExpr =
            Exp::seq(getEdgeTransfer(analysis, *Term, *Succ, 0), currentPath);
        graph.addEdge(fromId, addLabel(edgeExpr), blockIds.at(Succ));
      }
    }

    lotus::pathexpressions::PathExpressionComputer<int, int> computer(graph);
    unsigned starCounter = 0;
    llvm::BasicBlock *Entry = F.empty() ? nullptr : &F.getEntryBlock();
    prepared.artifacts.fullSummaryExpr =
        Entry ? translateRegex(computer.exprBetween(blockIds.at(Entry), exitId),
                               labels, starCounter)
              : Exp::term(D::one());
    prepared.artifacts.summaryExpr =
        makeSummaryEquationExpr(prepared.artifacts.fullSummaryExpr);
    for (auto &BB : F) {
      const std::string bSym = InterEngine<D, Analysis>::getBlockSymbol(&BB);
      prepared.artifacts.blockSummaryExprs.emplace(
          bSym, translateRegex(computer.exprBetween(blockIds.at(&BB), exitId),
                               labels, starCounter));
    }
    return prepared;
  }

public:
  struct PreparedBlockPropagation {
    const llvm::BasicBlock *block = nullptr;
    Fact entryFact;
    Val blockEndToExit;
    bool fact_approximate = false;
    std::vector<std::pair<std::string, Fact>> calleeExitFacts;
    bool saw_summary_overflow = false;
    bool approximated = false;
    long indirect_calls_seen = 0;
    long unresolved_indirect_calls = 0;
    long fallback_call_edges = 0;
    bool requires_external_callee_resolver = false;
  };

private:
  static PreparedBlockPropagation prepareBlockPropagation(
      llvm::Module &M, Analysis &analysis, llvm::BasicBlock &BB,
      typename InterEngine<D, Analysis>::CalleeCache &calleeCache,
      const Fact &exitFact, const std::unordered_map<Symbol, Val> &solvedMap,
      IndirectCallResolutionMode callResolutionMode) {
    PreparedBlockPropagation prepared;
    prepared.block = &BB;

    std::string bSym = InterEngine<D, Analysis>::getBlockSymbol(&BB);
    auto SolvedBlockIt = solvedMap.find(bSym);
    if (SolvedBlockIt == solvedMap.end())
      return prepared;

    ApproximationFlags local_flags;
    prepared.entryFact = applySummaryWithReporting(
        analysis, SolvedBlockIt->second, exitFact, local_flags, 0);
    prepared.fact_approximate =
        factIsApproximate(analysis, prepared.entryFact, 0);
    prepared.saw_summary_overflow = local_flags.used_summary_overflow;

    Val blockEndToExit = D::one();
    auto *Term = BB.getTerminator();
    if (Term && Term->getNumSuccessors() > 0) {
      bool first = true;
      for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
        auto *Succ = Term->getSuccessor(i);
        if (!Succ)
          continue;
        auto succSym = InterEngine<D, Analysis>::getBlockSymbol(Succ);
        auto SuccIt = solvedMap.find(succSym);
        if (SuccIt == solvedMap.end())
          continue;
        Val succToExit = D::extend(getEdgeTransfer(analysis, *Term, *Succ, 0),
                                   SuccIt->second);
        if (first) {
          blockEndToExit = succToExit;
          first = false;
        } else {
          blockEndToExit = D::combine(blockEndToExit, succToExit);
        }
      }
    }
    prepared.blockEndToExit = blockEndToExit;

    E currentPath = Exp::term(D::one());
    for (auto It = BB.rbegin(); It != BB.rend(); ++It) {
      llvm::Instruction &I = *It;
      if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
        if (CI->getCalledFunction() == nullptr)
          ++prepared.indirect_calls_seen;
        std::vector<llvm::Function *> Callees = calleeCache.get(*CI);
        if (!Callees.empty()) {
          Val currentPathVal = I0<D>::eval(false, solvedMap, currentPath);
          E callBranches = nullptr;
          for (llvm::Function *Callee : Callees) {
            std::string calleeSym =
                InterEngine<D, Analysis>::getFuncSymbol(Callee);
            Val afterCallToExit = D::extend(currentPathVal, blockEndToExit);
            Val calleeExitToExit =
                D::extend(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                          afterCallToExit);
            ApproximationFlags call_flags;
            Fact calleeExitFact = applySummaryWithReporting(
                analysis, calleeExitToExit, exitFact, call_flags, 0);
            prepared.saw_summary_overflow = prepared.saw_summary_overflow ||
                                            call_flags.used_summary_overflow;
            prepared.calleeExitFacts.emplace_back(std::move(calleeSym),
                                                  std::move(calleeExitFact));

            E branch = Exp::seq(
                getCallReturnTransfer(analysis, *CI, *Callee, 0), currentPath);
            branch = multiplyExpr(
                makeCallSummaryExpr(
                    InterEngine<D, Analysis>::getFuncSymbol(Callee)),
                branch);
            branch = Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                              branch);
            callBranches =
                callBranches ? Exp::ndet(callBranches, branch) : branch;
          }
          currentPath = callBranches;
        } else {
          if (CI->getCalledFunction() == nullptr) {
            ++prepared.unresolved_indirect_calls;
            if (callResolutionMode ==
                IndirectCallResolutionMode::CustomResolverRequired) {
              prepared.requires_external_callee_resolver = true;
              prepared.approximated = true;
            }
          }
          ++prepared.fallback_call_edges;
          currentPath =
              Exp::seq(getCallToReturnTransfer(analysis, *CI, 0), currentPath);
        }
      }
      currentPath = analysis.getTransfer(I, currentPath);
    }
    return prepared;
  }

public:
  static std::vector<llvm::Function *> getPossibleCallees(
      llvm::Module &M, const llvm::CallBase &Call,
      IndirectCallResolutionMode mode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible) {
    return InterEngine<D, Analysis>::getPossibleCallees(M, Call, mode);
  }

  static Result run(llvm::Module &M, Analysis &analysis, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible) {
    std::vector<std::pair<Symbol, E>> eqns;
    std::set<llvm::Function *> visited;
    std::unordered_map<std::string, FunctionKey> functionSymbols;
    std::unordered_map<std::string, llvm::Function *> functionsBySymbol;
    std::unordered_map<std::string, E> fullSummaryExprs;
    typename InterEngine<D, Analysis>::CalleeCache calleeCache(
        analysis, M, callResolutionMode);

    Result res;
    res.status.call_resolution_mode = callResolutionMode;
    res.status.open_world_unsound_mode =
        res.status.call_resolution_mode ==
        IndirectCallResolutionMode::ClosedWorldTypeCompatible;

    std::vector<llvm::Function *> entries =
        InterEngine<D, Analysis>::getEntryFunctions(M);
    std::vector<llvm::Function *> frontier(entries.begin(), entries.end());
    visited.insert(frontier.begin(), frontier.end());

    const auto ArtifactStart = std::chrono::steady_clock::now();
    while (!frontier.empty()) {
      std::vector<PreparedFunctionArtifacts> prepared(frontier.size());
      ThreadPool *pool = ThreadPool::get();
      const bool parallel_frontier =
          pool->workerCount() > 1 && frontier.size() > 1;
      if (parallel_frontier) {
        const std::size_t grain_size = detail::parallel_task_grain_size(
            frontier.size(), pool->workerCount(), 2);
        pool->parallelFor<std::size_t>(
            0, frontier.size(), grain_size, [&](std::size_t index) {
              prepared[index] = prepareFunctionRegexArtifacts(
                  M, *frontier[index], analysis, calleeCache,
                  res.status.call_resolution_mode);
            });
      } else {
        for (std::size_t index = 0; index < frontier.size(); ++index) {
          prepared[index] = prepareFunctionRegexArtifacts(
              M, *frontier[index], analysis, calleeCache,
              res.status.call_resolution_mode);
        }
      }

      std::vector<llvm::Function *> next_frontier;
      for (auto &item : prepared) {
        llvm::Function *F = item.function;
        std::string fSym = InterEngine<D, Analysis>::getFuncSymbol(F);
        functionSymbols[fSym] = {F};
        functionsBySymbol[fSym] = F;
        eqns.emplace_back(fSym, item.artifacts.summaryExpr);
        fullSummaryExprs.emplace(fSym, item.artifacts.fullSummaryExpr);
        for (const auto &blockExpr : item.artifacts.blockSummaryExprs)
          eqns.emplace_back(blockExpr.first, blockExpr.second);
        res.status.indirect_calls_seen += item.status_delta.indirect_calls_seen;
        res.status.unresolved_indirect_calls +=
            item.status_delta.unresolved_indirect_calls;
        res.status.fallback_call_edges += item.status_delta.fallback_call_edges;
        res.status.requires_external_callee_resolver =
            res.status.requires_external_callee_resolver ||
            item.status_delta.requires_external_callee_resolver;
        res.status.approximated =
            res.status.approximated || item.status_delta.approximated;
        for (llvm::Function *callee : item.discovered_callees) {
          if (callee && visited.insert(callee).second)
            next_frontier.push_back(callee);
        }
      }
      frontier.swap(next_frontier);
    }
    res.status.phase_artifact_construction_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      ArtifactStart)
            .count();

    auto rawRes = NPASolver<D>::solve(eqns, verbose, -1, linearStrategy);
    std::unordered_map<Symbol, Val> solvedMap;
    for (auto &p : rawRes.first)
      solvedMap[p.first] = p.second;

    res.status.summary_solve = rawRes.second;
    res.status.used_bounded_inner_solve =
        rawRes.second.hit_linear_limit || rawRes.second.hit_fixpoint_limit;
    res.status.approximated =
        !rawRes.second.converged || res.status.used_bounded_inner_solve;
    const auto SummaryMaterializationStart = std::chrono::steady_clock::now();
    for (const auto &entry : functionSymbols) {
      auto exprIt = fullSummaryExprs.find(entry.first);
      if (exprIt == fullSummaryExprs.end())
        continue;
      Val summary = I0<D>::eval(false, solvedMap, exprIt->second);
      if (summaryIsApproximate(analysis, summary, 0))
        res.status.approximated = true;
      res.summaries[entry.second] = summary;
    }
    res.status.phase_summary_materialization_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      SummaryMaterializationStart)
            .count();

    std::deque<llvm::Function *> worklist2;
    std::set<llvm::Function *> inWorklist2;
    std::unordered_map<std::string, Fact> funcExitFacts;
    std::unordered_map<std::string, size_t> funcUpdates;
    const long maxPropagationSteps = getMaxPropagationSteps(analysis, 0);
    long propagationSteps = 0;
    ApproximationFlags approx_flags;

    for (llvm::Function *Entry : entries) {
      std::string sym = InterEngine<D, Analysis>::getFuncSymbol(Entry);
      funcExitFacts[sym] = analysis.getExitValue(*Entry);
      worklist2.push_back(Entry);
      inWorklist2.insert(Entry);
    }

    const auto PropagationStart = std::chrono::steady_clock::now();
    while (!worklist2.empty()) {
      if (maxPropagationSteps >= 0 &&
          propagationSteps++ >= maxPropagationSteps) {
        res.status.propagation_hit_limit = true;
        res.status.propagation_converged = false;
        res.status.approximated = true;
        if (verbose)
          std::cerr << "[interproc-bwd] hit max propagation steps="
                    << maxPropagationSteps << "\n";
        break;
      }
      llvm::Function *F = worklist2.front();
      worklist2.pop_front();
      inWorklist2.erase(F);

      std::string fSym = InterEngine<D, Analysis>::getFuncSymbol(F);
      Fact exitFact = funcExitFacts[fSym];

      std::vector<llvm::BasicBlock *> blocks;
      blocks.reserve(F->size());
      for (auto &BB : *F)
        blocks.push_back(&BB);

      std::vector<PreparedBlockPropagation> prepared(blocks.size());
      ThreadPool *pool = ThreadPool::get();
      const bool parallel_blocks =
          pool->workerCount() > 1 && blocks.size() >= 4;
      if (parallel_blocks) {
        const std::size_t grain_size = detail::parallel_task_grain_size(
            blocks.size(), pool->workerCount(), 2);
        pool->parallelFor<std::size_t>(
            0, blocks.size(), grain_size, [&](std::size_t index) {
              prepared[index] = prepareBlockPropagation(
                  M, analysis, *blocks[index], calleeCache, exitFact, solvedMap,
                  res.status.call_resolution_mode);
            });
      } else {
        for (std::size_t index = 0; index < blocks.size(); ++index) {
          prepared[index] = prepareBlockPropagation(
              M, analysis, *blocks[index], calleeCache, exitFact, solvedMap,
              res.status.call_resolution_mode);
        }
      }

      for (const auto &block : prepared) {
        if (!block.block)
          continue;
        res.blockEntryFacts[{block.block}] = block.entryFact;
        if (block.fact_approximate || block.approximated)
          res.status.approximated = true;
        if (block.saw_summary_overflow)
          approx_flags.used_summary_overflow = true;
        res.status.indirect_calls_seen += block.indirect_calls_seen;
        res.status.unresolved_indirect_calls += block.unresolved_indirect_calls;
        res.status.fallback_call_edges += block.fallback_call_edges;
        if (block.requires_external_callee_resolver) {
          res.status.requires_external_callee_resolver = true;
          res.status.approximated = true;
        }

        for (const auto &callee_fact : block.calleeExitFacts) {
          auto Existing = funcExitFacts.find(callee_fact.first);
          if (Existing == funcExitFacts.end()) {
            funcExitFacts[callee_fact.first] = callee_fact.second;
            auto FnIt = functionsBySymbol.find(callee_fact.first);
            llvm::Function *Callee =
                FnIt != functionsBySymbol.end() ? FnIt->second : nullptr;
            if (Callee && inWorklist2.insert(Callee).second)
              worklist2.push_back(Callee);
          } else {
            Fact joined =
                analysis.joinFacts(Existing->second, callee_fact.second);
            if (!analysis.factsEqual(joined, Existing->second)) {
              size_t updateCount = ++funcUpdates[callee_fact.first];
              Fact widened =
                  widenFactsWithReporting(analysis, Existing->second, joined,
                                          updateCount, approx_flags, 0);
              if (hasCustomWidenFacts(analysis, 0))
                res.status.approximated = true;
              if (!analysis.factsEqual(widened, Existing->second)) {
                Existing->second = widened;
                auto FnIt = functionsBySymbol.find(callee_fact.first);
                llvm::Function *Callee =
                    FnIt != functionsBySymbol.end() ? FnIt->second : nullptr;
                if (Callee && inWorklist2.insert(Callee).second)
                  worklist2.push_back(Callee);
              }
            }
          }
        }
      }
    }
    res.status.phase_propagation_time =
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      PropagationStart)
            .count();
    if (approx_flags.used_summary_overflow) {
      res.status.used_summary_overflow = true;
      res.status.approximated = true;
    }
    if (approx_flags.used_fact_widening) {
      res.status.used_fact_widening = true;
      res.status.approximated = true;
    }
    res.status.propagation_steps = propagationSteps;
    res.status.overall_hit_limit =
        res.status.summary_solve.hit_limit || res.status.propagation_hit_limit;
    res.status.overall_converged = res.status.summary_solve.converged &&
                                   res.status.propagation_converged &&
                                   !res.status.used_summary_overflow &&
                                   !res.status.used_fact_widening;

    return res;
  }
};

} // namespace npa

#endif // NPA_LLVM_BACKWARD_INTER_ENGINE_H
