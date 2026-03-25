#ifndef NPA_INTERPROCEDURAL_ENGINE_H
#define NPA_INTERPROCEDURAL_ENGINE_H

#include "Dataflow/ControlFlow/IntraCFG.h"
#include "Dataflow/NPA/NPA.h"
#include "Utils/Algorithms/PathExpressions/PathExpressions.h"

#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace npa {

struct FunctionKey {
  const llvm::Function *function = nullptr;

  bool operator<(const FunctionKey &other) const {
    return function < other.function;
  }
};

struct BlockKey {
  const llvm::BasicBlock *block = nullptr;

  bool operator<(const BlockKey &other) const { return block < other.block; }
};

template <class D, class Analysis> class InterproceduralEngine {
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

public:
  static std::string getBlockSymbol(const llvm::BasicBlock *BB) {
    std::string s;
    s.reserve(1 + sizeof(BB));
    s.push_back('B');
    s.append(reinterpret_cast<const char *>(&BB), sizeof(BB));
    return s;
  }

  static std::string getFuncSymbol(const llvm::Function *F) {
    std::string s;
    s.reserve(1 + sizeof(F));
    s.push_back('F');
    s.append(reinterpret_cast<const char *>(&F), sizeof(F));
    return s;
  }

  static std::vector<llvm::Function *> getPossibleCallees(
      llvm::Module &M, const llvm::CallBase &Call,
      IndirectCallResolutionMode mode =
          IndirectCallResolutionMode::ClosedWorldTypeCompatible) {
    if (llvm::Function *Direct = Call.getCalledFunction()) {
      if (!Direct->isDeclaration())
        return {Direct};
      return {};
    }

    if (mode == IndirectCallResolutionMode::DeclaredOnlyFallback ||
        mode == IndirectCallResolutionMode::CustomResolverRequired) {
      return {};
    }

    if (auto *CalleeValue = Call.getCalledOperand()) {
      auto *Stripped = CalleeValue->stripPointerCasts();
      if (auto *Direct = llvm::dyn_cast<llvm::Function>(Stripped)) {
        if (!Direct->isDeclaration() &&
            isCallCompatibleWithFunction(Call, *Direct, M.getDataLayout()))
          return {Direct};
        return {};
      }
    }

    std::vector<llvm::Function *> matches;
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      if (isCallCompatibleWithFunction(Call, F, M.getDataLayout()))
        matches.push_back(&F);
    }
    return matches;
  }

  static bool typesAreCompatible(const llvm::Type *lhs, const llvm::Type *rhs) {
    if (lhs == rhs)
      return true;
    if (!lhs || !rhs)
      return false;
    if (lhs->isPointerTy() || rhs->isPointerTy())
      return false;
    if (lhs->isIntegerTy() && rhs->isIntegerTy())
      return lhs->getIntegerBitWidth() == rhs->getIntegerBitWidth();
    if (lhs->isFloatingPointTy() && rhs->isFloatingPointTy())
      return lhs->getTypeID() == rhs->getTypeID();
    return lhs->isVoidTy() && rhs->isVoidTy();
  }

  static bool isCallCompatibleWithFunction(const llvm::CallBase &Call,
                                           const llvm::Function &F,
                                           const llvm::DataLayout &DL) {
    if (Call.getCallingConv() != F.getCallingConv())
      return false;
    if (auto *CalledOperand = Call.getCalledOperand()) {
      if (!llvm::CastInst::isBitOrNoopPointerCastable(
              F.getType(), CalledOperand->getType(), DL)) {
        return false;
      }
    }
    llvm::FunctionType *calleeTy = F.getFunctionType();
    if (!typesAreCompatible(Call.getType(), calleeTy->getReturnType()))
      return false;
    if (!calleeTy->isVarArg() && Call.arg_size() != calleeTy->getNumParams())
      return false;
    if (calleeTy->isVarArg() && Call.arg_size() < calleeTy->getNumParams())
      return false;
    for (unsigned i = 0; i < calleeTy->getNumParams(); ++i) {
      llvm::Type *argTy = Call.getArgOperand(i)->getType();
      llvm::Type *paramTy = calleeTy->getParamType(i);
      if (argTy->isPointerTy() || paramTy->isPointerTy()) {
        if (!llvm::CastInst::isBitOrNoopPointerCastable(argTy, paramTy, DL))
          return false;
        continue;
      }
      if (!typesAreCompatible(argTy, paramTy)) {
        return false;
      }
    }
    return true;
  }

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
  static auto getPossibleCalleesForAnalysis(A &analysis, llvm::Module &M,
                                            const llvm::CallBase &call,
                                            IndirectCallResolutionMode, int)
      -> decltype(analysis.getPossibleCallees(M, call)) {
    return analysis.getPossibleCallees(M, call);
  }

  static std::vector<llvm::Function *>
  getPossibleCalleesForAnalysis(Analysis &, llvm::Module &M,
                                const llvm::CallBase &call,
                                IndirectCallResolutionMode mode, long) {
    return getPossibleCallees(M, call, mode);
  }

  template <typename A>
  static auto getCallResolutionMode(const A &analysis, int)
      -> decltype(analysis.getCallResolutionMode()) {
    return analysis.getCallResolutionMode();
  }

  static IndirectCallResolutionMode getCallResolutionMode(const Analysis &,
                                                          long) {
    return IndirectCallResolutionMode::ClosedWorldTypeCompatible;
  }

  template <typename A>
  static auto
  getCallFallbackTransfer(A &analysis, const llvm::CallBase &call,
                          const std::vector<llvm::Function *> &callees, int)
      -> decltype(analysis.getCallFallbackTransfer(call, callees)) {
    return analysis.getCallFallbackTransfer(call, callees);
  }

  static typename D::value_type
  getCallFallbackTransfer(Analysis &, const llvm::CallBase &,
                          const std::vector<llvm::Function *> &, long) {
    return D::zero();
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

  template <typename A>
  static auto buildBlockEntryExpr(A &analysis, llvm::BasicBlock &BB, E inExpr,
                                  int)
      -> decltype(analysis.buildBlockEntryExpr(BB, inExpr)) {
    return analysis.buildBlockEntryExpr(BB, inExpr);
  }

  static E buildBlockEntryExpr(Analysis &, llvm::BasicBlock &, E inExpr, long) {
    return inExpr;
  }

  static Symbol getSyntheticIncomingSymbol(const llvm::BasicBlock *BB) {
    std::string s;
    s.reserve(2 + sizeof(BB));
    s.push_back('I');
    s.push_back('N');
    s.append(reinterpret_cast<const char *>(&BB), sizeof(BB));
    return s;
  }

  static Val getBlockEntryTransfer(Analysis &analysis, llvm::BasicBlock &BB,
                                   const llvm::BasicBlock *Pred) {
    const Symbol incomingSym = getSyntheticIncomingSymbol(&BB);
    E entryExpr = buildBlockEntryExpr(analysis, BB, Exp::hole(incomingSym), 0);
    std::unordered_map<Symbol, Val> env;
    env[incomingSym] =
        Pred && Pred->getTerminator()
            ? getEdgeTransfer(analysis, *Pred->getTerminator(), BB, 0)
            : D::one();
    for (auto *OtherPred : predecessors(&BB)) {
      env[getBlockSymbol(OtherPred)] =
          (Pred != nullptr && OtherPred == Pred) ? D::one() : D::zero();
    }
    return I0<D>::eval(false, env, entryExpr);
  }

  struct FunctionRegexArtifacts {
    E summaryExpr;
    E fullSummaryExpr;
    std::unordered_map<std::string, E> blockEntryExprs;
    std::unordered_map<std::string, E> blockExitExprs;
  };

  static bool isZeroExpr(const E &expr) {
    return expr && expr->k == Exp::Term && D::equal(expr->c, D::zero());
  }

  static bool isOneExpr(const E &expr) {
    return expr && expr->k == Exp::Term && D::equal(expr->c, D::one());
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
    Symbol bound = "__regex_star_" + std::to_string(counter++);
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

  static E buildBlockBodyExpr(Analysis &analysis, llvm::Instruction &I,
                              E currentPath, llvm::Module &M,
                              std::deque<llvm::Function *> &worklist,
                              std::set<llvm::Function *> &visited,
                              AnalysisStatus &status,
                              IndirectCallResolutionMode callResolutionMode) {
    if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
      if (CI->getCalledFunction() == nullptr)
        ++status.indirect_calls_seen;
      std::vector<llvm::Function *> Callees = getPossibleCalleesForAnalysis(
          analysis, M, *CI, callResolutionMode, 0);
      if (!Callees.empty()) {
        E callBranches = nullptr;
        for (llvm::Function *Callee : Callees) {
          E branch = nullptr;
          if (Callee->isDeclaration()) {
            branch = Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              currentPath);
          } else {
            const Symbol callee_sym = getFuncSymbol(Callee);
            if (visited.insert(Callee).second)
              worklist.push_back(Callee);
            branch = Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                              currentPath);
            branch = multiplyExpr(makeCallSummaryExpr(callee_sym), branch);
            branch = Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                              branch);
          }
          callBranches = combineExpr(callBranches, branch);
        }
        Val fallbackTransfer =
            getCallFallbackTransfer(analysis, *CI, Callees, 0);
        if (!D::equal(fallbackTransfer, D::zero())) {
          ++status.fallback_call_edges;
          E fallbackBranch = Exp::seq(fallbackTransfer, currentPath);
          callBranches = combineExpr(callBranches, fallbackBranch);
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
        Val fallbackTransfer =
            getCallFallbackTransfer(analysis, *CI, Callees, 0);
        if (!D::equal(fallbackTransfer, D::zero())) {
          ++status.fallback_call_edges;
          currentPath = Exp::seq(fallbackTransfer, currentPath);
        } else
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

    ::dataflow::controlflow::LLVMIntraCFG CFG;
    std::unordered_map<const llvm::BasicBlock *, int> blockIds;
    int nextId = 1;
    for (auto &BB : F)
      blockIds[&BB] = nextId++;
    const int sourceId = 0;
    const int entryId = blockIds.at(&F.getEntryBlock());
    const int exitId = nextId;

    Graph graph;
    graph.addNode(sourceId);
    graph.addNode(exitId);

    std::vector<E> labels;
    labels.reserve(F.size() * 2U + 1U);
    std::unordered_map<std::string, E> blockBodyExprs;
    auto addLabel = [&](E expr) {
      labels.push_back(expr ? expr : Exp::term(D::zero()));
      return static_cast<int>(labels.size() - 1);
    };

    graph.addEdge(sourceId,
                  addLabel(Exp::term(getBlockEntryTransfer(
                      analysis, F.getEntryBlock(), nullptr))),
                  entryId);

    for (auto &BB : F) {
      const int fromId = blockIds.at(&BB);
      graph.addNode(fromId);

      E currentPath = Exp::term(D::one());
      for (auto &I : BB)
        currentPath = buildBlockBodyExpr(analysis, I, currentPath, M, worklist,
                                         visited, status, callResolutionMode);
      blockBodyExprs.emplace(getBlockSymbol(&BB), currentPath);

      auto *Term = BB.getTerminator();
      auto Succs =
          Term ? CFG.getSuccsOf(Term,
                                ::dataflow::controlflow::FlowDirection::Forward)
               : std::vector<llvm::Instruction *>{};
      if (Term == nullptr || Succs.empty()) {
        graph.addEdge(fromId, addLabel(currentPath), exitId);
        continue;
      }

      for (auto *SuccInst : Succs) {
        auto *Succ = SuccInst ? SuccInst->getParent() : nullptr;
        if (!Succ)
          continue;
        Val transfer = getBlockEntryTransfer(analysis, *Succ, &BB);
        E edgeExpr = Exp::seq(transfer, currentPath);
        graph.addEdge(fromId, addLabel(edgeExpr), blockIds.at(Succ));
      }
    }

    lotus::pathexpressions::PathExpressionComputer<int, int> computer(graph);
    unsigned starCounter = 0;

    FunctionRegexArtifacts out;
    out.fullSummaryExpr = translateRegex(computer.exprBetween(sourceId, exitId),
                                         labels, starCounter);
    out.summaryExpr = makeSummaryEquationExpr(out.fullSummaryExpr);
    for (auto &BB : F) {
      const std::string bSym = getBlockSymbol(&BB);
      E entryExpr =
          translateRegex(computer.exprBetween(sourceId, blockIds.at(&BB)),
                         labels, starCounter);
      out.blockEntryExprs.emplace(bSym, entryExpr);
      out.blockExitExprs.emplace(
          bSym, multiplyExpr(blockBodyExprs.at(bSym), entryExpr));
    }
    return out;
  }

public:
  static std::vector<llvm::Function *> getEntryFunctions(llvm::Module &M) {
    if (llvm::Function *Main = M.getFunction("main"))
      return {Main};

    std::unordered_set<const llvm::Function *> called;
    std::vector<llvm::Function *> defined;
    for (auto &F : M) {
      if (F.isDeclaration())
        continue;
      defined.push_back(&F);
      for (auto &BB : F) {
        for (auto &I : BB) {
          auto *CB = llvm::dyn_cast<llvm::CallBase>(&I);
          if (!CB)
            continue;
          for (llvm::Function *Callee : getPossibleCallees(M, *CB)) {
            if (!Callee->isDeclaration())
              called.insert(Callee);
          }
        }
      }
    }

    std::vector<llvm::Function *> roots;
    for (llvm::Function *F : defined) {
      if (!called.count(F))
        roots.push_back(F);
    }
    return roots.empty() ? defined : roots;
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
    std::unordered_map<std::string, E> blockEntryExprs;
    std::unordered_map<std::string, E> blockExitExprs;

    Result res;
    res.status.call_resolution_mode = callResolutionMode;
    res.status.open_world_unsound_mode =
        res.status.call_resolution_mode ==
        IndirectCallResolutionMode::ClosedWorldTypeCompatible;

    std::vector<llvm::Function *> entries = getEntryFunctions(M);
    for (llvm::Function *Entry : entries) {
      worklist.push_back(Entry);
      visited.insert(Entry);
    }

    while (!worklist.empty()) {
      llvm::Function *F = worklist.front();
      worklist.pop_front();

      std::string fSym = getFuncSymbol(F);
      functionSymbols[fSym] = {F};
      auto artifacts = buildFunctionRegexArtifacts(
          M, *F, analysis, worklist, visited, res.status,
          res.status.call_resolution_mode);
      eqns.emplace_back(fSym, artifacts.summaryExpr);
      fullSummaryExprs.emplace(fSym, artifacts.fullSummaryExpr);
      for (auto &blockExpr : artifacts.blockEntryExprs)
        blockEntryExprs.emplace(blockExpr.first, blockExpr.second);
      for (auto &blockExpr : artifacts.blockExitExprs)
        blockExitExprs.emplace(blockExpr.first, blockExpr.second);
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
    std::unordered_map<std::string, Fact> funcInput;
    std::unordered_map<std::string, size_t> funcUpdates;
    const long maxPropagationSteps = getMaxPropagationSteps(analysis, 0);
    long propagationSteps = 0;
    ApproximationFlags approx_flags;

    for (llvm::Function *Entry : entries) {
      std::string sym = getFuncSymbol(Entry);
      funcInput[sym] = analysis.getEntryValue();
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
          std::cerr << "[interproc-fwd] hit max propagation steps="
                    << maxPropagationSteps << "\n";
        break;
      }
      llvm::Function *F = worklist2.front();
      worklist2.pop_front();
      inWorklist2.erase(F);

      std::string fSym = getFuncSymbol(F);
      Fact inputVal = funcInput[fSym];

      for (auto &BB : *F) {
        std::string bSym = getBlockSymbol(&BB);
        auto blockExprIt = blockEntryExprs.find(bSym);
        if (blockExprIt == blockEntryExprs.end())
          continue;
        Val entryToBlockStart = D::zero();
        if (llvm::isa<llvm::PHINode>(BB.begin())) {
          auto blockExpr =
              buildBlockEntryExpr(analysis, BB, Exp::term(D::zero()), 0);
          std::unordered_map<Symbol, Val> env = solvedMap;
          for (auto *Pred : predecessors(&BB)) {
            const std::string predSym = getBlockSymbol(Pred);
            auto predIt = blockExitExprs.find(predSym);
            if (predIt == blockExitExprs.end())
              continue;
            env[predSym] = I0<D>::eval(false, solvedMap, predIt->second);
          }
          entryToBlockStart = I0<D>::eval(false, env, blockExpr);
        } else {
          entryToBlockStart =
              I0<D>::eval(false, solvedMap, blockExprIt->second);
        }

        auto blockEntryFact = applySummaryWithReporting(
            analysis, entryToBlockStart, inputVal, approx_flags, 0);
        if (factIsApproximate(analysis, blockEntryFact, 0))
          res.status.approximated = true;
        res.blockEntryFacts[{&BB}] = blockEntryFact;

        E currentPath = Exp::term(D::one());

        for (auto &I : BB) {
          if (auto *CI = llvm::dyn_cast<llvm::CallBase>(&I)) {
            if (CI->getCalledFunction() == nullptr)
              ++res.status.indirect_calls_seen;
            std::vector<llvm::Function *> Callees =
                getPossibleCalleesForAnalysis(
                    analysis, M, *CI, res.status.call_resolution_mode, 0);
            if (!Callees.empty()) {
              Val currentPathVal = I0<D>::eval(false, solvedMap, currentPath);
              E callBranches = nullptr;
              for (llvm::Function *Callee : Callees) {
                E branch = nullptr;
                if (Callee->isDeclaration()) {
                  branch =
                      Exp::seq(getCallReturnTransfer(analysis, *CI, *Callee, 0),
                               currentPath);
                } else {
                  std::string calleeFSym = getFuncSymbol(Callee);

                  Val callEntry =
                      D::extend(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                                currentPathVal);
                  Val totalToCall = D::extend(callEntry, entryToBlockStart);

                  auto factAtCall = applySummaryWithReporting(
                      analysis, totalToCall, inputVal, approx_flags, 0);

                  if (!funcInput.count(calleeFSym)) {
                    funcInput[calleeFSym] = factAtCall;
                    if (inWorklist2.insert(Callee).second)
                      worklist2.push_back(Callee);
                  } else {
                    auto oldVal = funcInput[calleeFSym];
                    auto newVal = analysis.joinFacts(oldVal, factAtCall);
                    if (!analysis.factsEqual(oldVal, newVal)) {
                      size_t updateCount = ++funcUpdates[calleeFSym];
                      Fact widened = widenFactsWithReporting(
                          analysis, oldVal, newVal, updateCount, approx_flags,
                          0);
                      if (hasCustomWidenFacts(analysis, 0))
                        res.status.approximated = true;
                      if (!analysis.factsEqual(oldVal, widened)) {
                        funcInput[calleeFSym] = widened;
                        if (inWorklist2.insert(Callee).second)
                          worklist2.push_back(Callee);
                      }
                    }
                  }

                  branch =
                      Exp::seq(getCallEntryTransfer(analysis, *CI, *Callee, 0),
                               currentPath);
                  branch =
                      multiplyExpr(makeCallSummaryExpr(calleeFSym), branch);
                  branch = Exp::seq(
                      getCallReturnTransfer(analysis, *CI, *Callee, 0), branch);
                }
                callBranches =
                    callBranches ? Exp::ndet(callBranches, branch) : branch;
              }
              Val fallbackTransfer =
                  getCallFallbackTransfer(analysis, *CI, Callees, 0);
              if (!D::equal(fallbackTransfer, D::zero())) {
                ++res.status.fallback_call_edges;
                E fallbackBranch = Exp::seq(fallbackTransfer, currentPath);
                callBranches = callBranches
                                   ? Exp::ndet(callBranches, fallbackBranch)
                                   : fallbackBranch;
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
              Val fallbackTransfer =
                  getCallFallbackTransfer(analysis, *CI, Callees, 0);
              if (!D::equal(fallbackTransfer, D::zero())) {
                ++res.status.fallback_call_edges;
                currentPath = Exp::seq(fallbackTransfer, currentPath);
              } else
                currentPath = Exp::seq(
                    getCallToReturnTransfer(analysis, *CI, 0), currentPath);
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

#endif
