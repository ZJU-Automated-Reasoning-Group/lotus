#ifndef DATAFLOW_APA_SOLVER_FORWARDINTERSUMMARYSOLVER_H_
#define DATAFLOW_APA_SOLVER_FORWARDINTERSUMMARYSOLVER_H_

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Solver/InterSummaryTransfer.h"
#include "Dataflow/APA/Solver/PathSummaryEquationSolver.h"
#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elimination {

// Forward context-sensitive interprocedural solver based on one global graph of
// path-summary equations. Nodes are instruction/context pairs. Edges are
// first-class interprocedural transfer atoms, so calls are represented as:
//
//   callsite --RawNormal.CallEntry--> callee entry
//   callsite --RawNormal.CallToRet--> return site
//   callee exit --RawNormal.ReturnExit--> return site
//
// The resulting equation graph is solved with PathSummaryEquationSolver, which
// closes recursive SCCs with Star expressions and evaluates SCCs in dependency
// order.
template <typename AnalysisTypesT, unsigned K>
class ForwardInterSummarySolver final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisTypesT>;
  using fact_t = typename AnalysisTypesT::fact_t;
  using n_t = typename AnalysisTypesT::n_t;
  using f_t = typename AnalysisTypesT::f_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;
  using i_t = typename AnalysisTypesT::i_t;
  using result_t = InterDataFlowResultT<K, fact_t, transfer_t, n_t>;
  using Context = mono::CallStringCTX<n_t, K>;

  struct ContextKey final {
    n_t Inst{};
    Context Ctx;

    bool operator<(const ContextKey &Other) const {
      if (Inst != Other.Inst) {
        return Inst < Other.Inst;
      }
      return Ctx < Other.Ctx;
    }
  };

  using atom_t = InterSummaryTransferAtom<AnalysisTypesT>;
  using summary_graph_t = PathSummaryEquationGraph<ContextKey, atom_t>;
  using expr_ref_t = typename summary_graph_t::expr_ref_t;

  struct Diagnostics final {
    PathSummaryEquationDiagnostics equation_graph;
    std::size_t discovered_context_node_count = 0;
    std::size_t seed_count = 0;
  };

  explicit ForwardInterSummarySolver(ProblemTy &Problem,
                                     PathSummaryEquationOptions Options = {})
      : Problem(Problem), Options(Options) {}

  SolveStatus solve() {
    const auto *ICFPtr = Problem.getICFG();
    if (ICFPtr == nullptr ||
        Problem.direction() != dataflow::controlflow::FlowDirection::Forward) {
      HaveResult = false;
      return LastStatus = SolveStatus::InvalidProblem;
    }
    ICF = ICFPtr;
    SeedFacts = Problem.initialSeeds();

    Graph = summary_graph_t{};
    Discovered.clear();
    InitialFact = Problem.bottom();
    HaveInitialFact = false;

    discoverEquationGraph();
    auto SolverOptions = Options;
    SolverOptions.Direction = PathSummaryEquationDirection::ForwardPath;
    PathSummaryEquationSolver<ContextKey, atom_t> Solver(Graph, SolverOptions);
    auto Summary = Solver.solve();
    DiagnosticsValue.equation_graph = Summary.diagnostics();

    Result = result_t{};
    Result.setMissingFactFallback(Problem.bottom());
    evaluateSummaries(Summary);
    Result.setSolveStatus(SolveStatus::Ok);
    HaveResult = true;
    return LastStatus = SolveStatus::Ok;
  }

  const result_t *getResults() const { return HaveResult ? &Result : nullptr; }
  SolveStatus getLastStatus() const { return LastStatus; }
  const Diagnostics &diagnostics() const { return DiagnosticsValue; }

  InterSummarySolveDiagnostics resultDiagnostics() const {
    InterSummarySolveDiagnostics Out;
    Out.discovered_context_node_count =
        DiagnosticsValue.discovered_context_node_count;
    Out.seed_count = DiagnosticsValue.seed_count;
    Out.equation_node_count = DiagnosticsValue.equation_graph.node_count;
    Out.equation_edge_count = DiagnosticsValue.equation_graph.edge_count;
    Out.scc_count = DiagnosticsValue.equation_graph.scc_count;
    Out.cyclic_scc_count = DiagnosticsValue.equation_graph.cyclic_scc_count;
    return Out;
  }

private:
  expr_ref_t rawNormalExpr(transfer_t Transfer) {
    return Graph.exprs().atom(atom_t::rawNormal(std::move(Transfer)));
  }

  expr_ref_t concat(const expr_ref_t &Lhs, const expr_ref_t &Rhs) {
    return Graph.exprs().concat(Lhs, Rhs);
  }

  void rememberInitialFact(const fact_t &Fact) {
    if (!HaveInitialFact) {
      InitialFact = Fact;
      HaveInitialFact = true;
      return;
    }
    InitialFact = Problem.join(InitialFact, Fact);
  }

  void enqueue(ContextKey Key, std::deque<ContextKey> &Worklist) {
    if (Key.Inst == n_t{}) {
      return;
    }
    if (Discovered.insert(Key).second) {
      Graph.addNode(Key);
      Worklist.push_back(std::move(Key));
    }
  }

  void addSeed(ContextKey Key, const fact_t &Fact,
               std::deque<ContextKey> &Worklist) {
    enqueue(Key, Worklist);
    Graph.setBase(Key, Graph.exprs().one());
    rememberInitialFact(Fact);
    ++DiagnosticsValue.seed_count;
  }

  void addEquationEdge(const ContextKey &Target, const ContextKey &Source,
                       expr_ref_t Weight, std::deque<ContextKey> &Worklist) {
    enqueue(Source, Worklist);
    enqueue(Target, Worklist);
    Graph.addEdge(Source, Target, std::move(Weight));
  }

  void discoverEquationGraph() {
    std::deque<ContextKey> Worklist;
    Context EmptyCtx;

    for (const auto &Seed : SeedFacts) {
      addSeed({Seed.first, EmptyCtx}, Seed.second, Worklist);
    }

    if (SeedFacts.empty()) {
      for (auto Entry : Problem.getEntryPoints()) {
        if (Entry == f_t{}) {
          continue;
        }
        auto Starts = ICF->getStartPointsOf(Entry);
        if (!Starts.empty() && Starts.front() != n_t{}) {
          addSeed({Starts.front(), EmptyCtx}, Problem.bottom(), Worklist);
        }
      }
    }

    while (!Worklist.empty()) {
      auto Key = Worklist.front();
      Worklist.pop_front();
      discoverSuccessors(Key, Worklist);
    }

    DiagnosticsValue.discovered_context_node_count = Discovered.size();
  }

  void discoverSuccessors(const ContextKey &Key,
                          std::deque<ContextKey> &Worklist) {
    auto Inst = Key.Inst;
    if (Inst == n_t{}) {
      return;
    }

    if (ICF->isCallSite(Inst)) {
      discoverCallSuccessors(Key, Worklist);
      return;
    }

    for (auto Succ :
         ICF->getSuccsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
      if (Succ == n_t{}) {
        continue;
      }
      auto Weight = rawNormalExpr(Problem.edgeTransfer(Inst, Succ));
      addEquationEdge({Succ, Key.Ctx}, Key, Weight, Worklist);
    }
  }

  void discoverCallSuccessors(const ContextKey &Key,
                              std::deque<ContextKey> &Worklist) {
    const auto CallSite = Key.Inst;
    const auto Callees = ICF->getCalleesOfCallAt(CallSite);

    for (auto RetSite : ICF->getReturnSitesOfCallAt(CallSite)) {
      if (RetSite == n_t{}) {
        continue;
      }
      auto Transfer = Problem.edgeTransfer(CallSite, RetSite);
      auto Bypass = rawNormalExpr(Transfer);
      if (Problem.transferSuccessor(Transfer) != n_t{}) {
        auto CallToRet =
            Graph.exprs().atom(atom_t::callToRet(CallSite, RetSite, Callees));
        Bypass = concat(Bypass, CallToRet);
      }
      addEquationEdge({RetSite, Key.Ctx}, Key, Bypass, Worklist);
    }

    Context CalleeCtx = Key.Ctx;
    CalleeCtx.push_back(CallSite);
    for (auto Callee : Callees) {
      auto Starts = ICF->getStartPointsOf(Callee);
      auto Exits = ICF->getExitPointsOf(Callee);
      if (Starts.empty() || Exits.empty()) {
        continue;
      }

      auto Enter = Graph.exprs().atom(atom_t::callEntry(CallSite, Callee));
      auto RawCall = rawNormalExpr(Problem.edgeTransfer(CallSite, n_t{}));
      addEquationEdge({Starts.front(), CalleeCtx}, Key, concat(RawCall, Enter),
                      Worklist);

      for (auto Exit : Exits) {
        if (Exit == n_t{}) {
          continue;
        }
        ContextKey ExitKey{Exit, CalleeCtx};
        enqueue(ExitKey, Worklist);
        auto RawExit = rawNormalExpr(Problem.edgeTransfer(Exit, n_t{}));
        for (auto RetSite : ICF->getReturnSitesOfCallAt(CallSite)) {
          if (RetSite == n_t{}) {
            continue;
          }
          auto Return = Graph.exprs().atom(
              atom_t::returnExit(CallSite, Callee, Exit, RetSite));
          addEquationEdge({RetSite, Key.Ctx}, ExitKey, concat(RawExit, Return),
                          Worklist);
        }
      }
    }
  }

  void evaluateSummaries(
      const PathSummaryEquationResult<ContextKey, atom_t> &Summary) {
    if (!HaveInitialFact) {
      InitialFact = Problem.bottom();
    }

    for (const auto &Entry : Summary.summaries()) {
      InterSummaryTransferEvaluator<AnalysisTypesT, K> Evaluator(
          Problem, *ICF, Result, Entry.first.Ctx);
      auto In = Evaluator.evaluateExpr(Entry.second, InitialFact);
      Result.IN(Entry.first.Inst, Entry.first.Ctx) = In;

      auto Out = Problem.applyTransfer(
          Problem.edgeTransfer(Entry.first.Inst, n_t{}), In);
      Result.OUT(Entry.first.Inst, Entry.first.Ctx) = std::move(Out);
    }
  }

  ProblemTy &Problem;
  PathSummaryEquationOptions Options;
  const i_t *ICF = nullptr;
  summary_graph_t Graph;
  std::set<ContextKey> Discovered;
  std::unordered_map<n_t, fact_t> SeedFacts;
  fact_t InitialFact{};
  bool HaveInitialFact = false;
  result_t Result;
  bool HaveResult = false;
  SolveStatus LastStatus = SolveStatus::Ok;
  Diagnostics DiagnosticsValue;
};

} // namespace elimination

#endif // DATAFLOW_APA_SOLVER_FORWARDINTERSUMMARYSOLVER_H_
