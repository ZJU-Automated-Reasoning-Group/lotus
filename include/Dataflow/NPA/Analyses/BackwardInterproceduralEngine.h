#ifndef NPA_BACKWARD_INTERPROCEDURAL_ENGINE_H
#define NPA_BACKWARD_INTERPROCEDURAL_ENGINE_H

#include "Dataflow/NPA/Analyses/InterproceduralEngine.h"
#include "Dataflow/NPA/NPA.h"
#include "Utils/Algorithms/PathExpressions/PathExpressions.h"

#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace npa {

template <class D, class Analysis> class BackwardInterproceduralEngine {
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
                                        const Fact &fact,
                                        ApproximationFlags &, long) {
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

  static E buildBlockBodyExpr(Analysis &analysis, llvm::Instruction &I,
                              E currentPath, llvm::Module &M,
                              std::deque<llvm::Function *> &worklist,
                              std::set<llvm::Function *> &visited,
                              AnalysisStatus &status,
                              IndirectCallResolutionMode callResolutionMode) {
    if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
      if (CI->getCalledFunction() == nullptr)
        ++status.indirect_calls_seen;
      std::vector<llvm::Function *> Callees =
          getPossibleCallees(M, *CI, callResolutionMode);
      if (!Callees.empty()) {
        E callBranches = nullptr;
        for (llvm::Function *Callee : Callees) {
          const Symbol callee_sym =
              InterproceduralEngine<D, Analysis>::getFuncSymbol(Callee);
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

  static FunctionRegexArtifacts buildFunctionRegexArtifacts(
      llvm::Module &M, llvm::Function &F, Analysis &analysis,
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
            buildBlockBodyExpr(analysis, *It, currentPath, M, worklist, visited,
                               status, callResolutionMode);

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
      const std::string bSym =
          InterproceduralEngine<D, Analysis>::getBlockSymbol(&BB);
      out.blockSummaryExprs.emplace(
          bSym, translateRegex(computer.exprBetween(blockIds.at(&BB), exitId),
                               labels, starCounter));
    }
    return out;
  }

public:
  static std::vector<llvm::Function *> getPossibleCallees(
      llvm::Module &M, const llvm::CallBase &Call,
      IndirectCallResolutionMode mode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible) {
    return InterproceduralEngine<D, Analysis>::getPossibleCallees(M, Call,
                                                                  mode);
  }

  static Result run(llvm::Module &M, Analysis &analysis, bool verbose = false,
                    LinearStrategy linearStrategy = LinearStrategy::SCC,
                    IndirectCallResolutionMode callResolutionMode =
                        IndirectCallResolutionMode::ClosedWorldTypeCompatible) {
    std::vector<std::pair<Symbol, E>> eqns;
    std::deque<llvm::Function *> worklist;
    std::set<llvm::Function *> visited;
    std::unordered_map<std::string, FunctionKey> functionSymbols;
    std::unordered_map<std::string, E> fullSummaryExprs;

    Result res;
    res.status.call_resolution_mode = callResolutionMode;
    res.status.open_world_unsound_mode =
        res.status.call_resolution_mode ==
        IndirectCallResolutionMode::ClosedWorldTypeCompatible;

    std::vector<llvm::Function *> entries =
        InterproceduralEngine<D, Analysis>::getEntryFunctions(M);
    for (llvm::Function *Entry : entries) {
      worklist.push_back(Entry);
      visited.insert(Entry);
    }

    while (!worklist.empty()) {
      llvm::Function *F = worklist.front();
      worklist.pop_front();

      std::string fSym = InterproceduralEngine<D, Analysis>::getFuncSymbol(F);
      functionSymbols[fSym] = {F};
      auto artifacts = buildFunctionRegexArtifacts(
          M, *F, analysis, worklist, visited, res.status,
          res.status.call_resolution_mode);
      eqns.emplace_back(fSym, artifacts.summaryExpr);
      fullSummaryExprs.emplace(fSym, artifacts.fullSummaryExpr);
      for (const auto &blockExpr : artifacts.blockSummaryExprs)
        eqns.emplace_back(blockExpr.first, blockExpr.second);
    }

    auto rawRes = NewtonSolver<D>::solve(eqns, verbose, -1, linearStrategy);
    std::unordered_map<Symbol, Val> solvedMap;
    for (auto &p : rawRes.first)
      solvedMap[p.first] = p.second;

    res.status.summary_solve = rawRes.second;
    res.status.used_bounded_inner_solve =
        rawRes.second.hit_linear_limit || rawRes.second.hit_fixpoint_limit;
    res.status.approximated =
        !rawRes.second.converged || res.status.used_bounded_inner_solve;
    for (const auto &entry : functionSymbols) {
      auto exprIt = fullSummaryExprs.find(entry.first);
      if (exprIt == fullSummaryExprs.end())
        continue;
      Val summary = I0<D>::eval(false, solvedMap, exprIt->second);
      if (summaryIsApproximate(analysis, summary, 0))
        res.status.approximated = true;
      res.summaries[entry.second] = summary;
    }

    std::deque<llvm::Function *> worklist2;
    std::set<llvm::Function *> inWorklist2;
    std::unordered_map<std::string, Fact> funcExitFacts;
    std::unordered_map<std::string, size_t> funcUpdates;
    const long maxPropagationSteps = getMaxPropagationSteps(analysis, 0);
    long propagationSteps = 0;
    ApproximationFlags approx_flags;

    for (llvm::Function *Entry : entries) {
      std::string sym =
          InterproceduralEngine<D, Analysis>::getFuncSymbol(Entry);
      funcExitFacts[sym] = analysis.getExitValue(*Entry);
      worklist2.push_back(Entry);
      inWorklist2.insert(Entry);
    }

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

      std::string fSym = InterproceduralEngine<D, Analysis>::getFuncSymbol(F);
      Fact exitFact = funcExitFacts[fSym];

      for (auto &BB : *F) {
        std::string bSym =
            InterproceduralEngine<D, Analysis>::getBlockSymbol(&BB);
        auto SolvedBlockIt = solvedMap.find(bSym);
        if (SolvedBlockIt == solvedMap.end())
          continue;

        res.blockEntryFacts[{&BB}] = applySummaryWithReporting(
            analysis, SolvedBlockIt->second, exitFact, approx_flags, 0);
        if (factIsApproximate(analysis, res.blockEntryFacts[{&BB}], 0))
          res.status.approximated = true;

        Val blockEndToExit = D::one();
        auto *Term = BB.getTerminator();
        if (Term && Term->getNumSuccessors() > 0) {
          bool first = true;
          for (unsigned i = 0; i < Term->getNumSuccessors(); ++i) {
            auto *Succ = Term->getSuccessor(i);
            if (!Succ)
              continue;
            auto succSym =
                InterproceduralEngine<D, Analysis>::getBlockSymbol(Succ);
            auto SuccIt = solvedMap.find(succSym);
            if (SuccIt == solvedMap.end())
              continue;
            Val succToExit = D::extend(
                getEdgeTransfer(analysis, *Term, *Succ, 0), SuccIt->second);
            if (first) {
              blockEndToExit = succToExit;
              first = false;
            } else {
              blockEndToExit = D::combine(blockEndToExit, succToExit);
            }
          }
        }

        E currentPath = Exp::term(D::one());
        for (auto It = BB.rbegin(); It != BB.rend(); ++It) {
          llvm::Instruction &I = *It;
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (CI->getCalledFunction() == nullptr)
              ++res.status.indirect_calls_seen;
            std::vector<llvm::Function *> Callees =
                getPossibleCallees(M, *CI, res.status.call_resolution_mode);
            if (!Callees.empty()) {
              Val currentPathVal = I0<D>::eval(false, solvedMap, currentPath);
              E callBranches = nullptr;
              for (llvm::Function *Callee : Callees) {
                std::string calleeSym =
                    InterproceduralEngine<D, Analysis>::getFuncSymbol(Callee);
                Val afterCallToExit = D::extend(currentPathVal, blockEndToExit);
                Val calleeExitToExit =
                    D::extend(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              afterCallToExit);
                Fact calleeExitFact = applySummaryWithReporting(
                    analysis, calleeExitToExit, exitFact, approx_flags, 0);

                auto Existing = funcExitFacts.find(calleeSym);
                if (Existing == funcExitFacts.end()) {
                  funcExitFacts[calleeSym] = calleeExitFact;
                  if (inWorklist2.insert(Callee).second)
                    worklist2.push_back(Callee);
                } else {
                  Fact joined =
                      analysis.joinFacts(Existing->second, calleeExitFact);
                  if (!analysis.factsEqual(joined, Existing->second)) {
                    size_t updateCount = ++funcUpdates[calleeSym];
                    Fact widened = widenFactsWithReporting(
                        analysis, Existing->second, joined, updateCount,
                        approx_flags, 0);
                    if (hasCustomWidenFacts(analysis, 0))
                      res.status.approximated = true;
                    if (!analysis.factsEqual(widened, Existing->second)) {
                      Existing->second = widened;
                      if (inWorklist2.insert(Callee).second)
                        worklist2.push_back(Callee);
                    }
                  }
                }

                E branch =
                    Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                             currentPath);
                branch = multiplyExpr(makeCallSummaryExpr(calleeSym), branch);
                branch = Exp::seq(
                    getCallEntryTransfer(analysis, *CI, *Callee, 0), branch);
                callBranches =
                    callBranches ? Exp::ndet(callBranches, branch) : branch;
              }
              currentPath = callBranches;
            } else {
              if (CI->getCalledFunction() == nullptr) {
                ++res.status.unresolved_indirect_calls;
                if (res.status.call_resolution_mode ==
                    IndirectCallResolutionMode::CustomResolverRequired) {
                  res.status.requires_external_callee_resolver = true;
                  res.status.approximated = true;
                }
              }
              ++res.status.fallback_call_edges;
              currentPath = Exp::seq(getCallToReturnTransfer(analysis, *CI, 0),
                                     currentPath);
            }
          }
          currentPath = analysis.getTransfer(I, currentPath);
        }
      }
    }
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
    res.status.overall_converged =
        res.status.summary_solve.converged && res.status.propagation_converged &&
        !res.status.used_summary_overflow && !res.status.used_fact_widening;

    return res;
  }
};

} // namespace npa

#endif // NPA_BACKWARD_INTERPROCEDURAL_ENGINE_H
