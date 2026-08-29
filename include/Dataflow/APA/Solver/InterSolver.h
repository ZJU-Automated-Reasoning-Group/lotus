#ifndef DATAFLOW_APA_SOLVER_INTERSOLVER_H_
#define DATAFLOW_APA_SOLVER_INTERSOLVER_H_

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/Solver/InterSummaryTransfer.h"
#include "Dataflow/APA/Solver/Solver.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <deque>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace elimination {

template <typename AnalysisTypesT, unsigned K>
class InterEliminationSolver final {
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

  explicit InterEliminationSolver(ProblemTy &Problem) : Problem(Problem) {}

  SolveStatus solve() {
    loadSeedFacts();
    const auto *ICF = Problem.getICFG();
    if (ICF == nullptr) {
      HaveResult = false;
      return LastStatus = SolveStatus::InvalidProblem;
    }

    Result = result_t{};
    Result.setMissingFactFallback(Problem.bottom());
    HaveResult = true;

    std::deque<ContextKey> Worklist;
    std::set<ContextKey> InQueue;

    auto Enqueue = [&](ContextKey Key) {
      if (InQueue.insert(Key).second) {
        Worklist.push_back(std::move(Key));
      }
    };

    Context EmptyCtx;
    for (const auto &Seed : SeedFacts) {
      Result.IN(Seed.first, EmptyCtx) = Seed.second;
      Enqueue({Seed.first, EmptyCtx});
    }

    if (SeedFacts.empty()) {
      for (auto Entry : Problem.getEntryPoints()) {
        if (Entry == f_t{}) {
          continue;
        }
        if (Problem.direction() ==
            ::dataflow::controlflow::FlowDirection::Backward) {
          for (auto Exit : ICF->getExitPointsOf(Entry)) {
            if (Exit != n_t{}) {
              Enqueue({Exit, EmptyCtx});
            }
          }
        } else {
          auto Starts = ICF->getStartPointsOf(Entry);
          if (!Starts.empty() && Starts.front() != n_t{}) {
            Enqueue({Starts.front(), EmptyCtx});
          }
        }
      }
    }

    while (!Worklist.empty()) {
      auto Key = Worklist.front();
      Worklist.pop_front();
      InQueue.erase(Key);

      std::vector<ContextKey> Frontier;
      if (!solveProcedureForContext(Key, *ICF, Frontier)) {
        continue;
      }

      for (const auto &Succ : Frontier) {
        Enqueue(Succ);
      }
    }

    Result.setSolveStatus(SolveStatus::Ok);
    return LastStatus = SolveStatus::Ok;
  }

  const result_t *getResults() const { return HaveResult ? &Result : nullptr; }
  SolveStatus getLastStatus() const { return LastStatus; }

private:
  class ProcedureProblemAdapter final
      : public IntraEliminationProblem<AnalysisTypesT> {
  public:
    ProcedureProblemAdapter(ProblemTy &Problem, f_t Function,
                            const Context &Ctx, const result_t &Result,
                            const i_t &ICF, const fact_t &EntryFact)
        : Problem(Problem), Function(Function), Ctx(Ctx), Result(Result),
          ICF(ICF), EntryFact(EntryFact) {}

    std::vector<n_t> nodes() const override {
      if (Function == f_t{}) {
        return {};
      }
      return ICF.getAllInstructionsOf(Function);
    }

    n_t entry() const override {
      if (Function == f_t{}) {
        return n_t{};
      }
      if (Problem.direction() ==
          ::dataflow::controlflow::FlowDirection::Backward) {
        auto Exits = ICF.getExitPointsOf(Function);
        return Exits.empty() ? n_t{} : Exits.front();
      }
      auto Starts = ICF.getStartPointsOf(Function);
      return Starts.empty() ? n_t{} : Starts.front();
    }

    std::vector<n_t> succs(n_t Node) const override {
      return ICF.getSuccsOf(Node, Problem.direction());
    }

    transfer_t edgeTransfer(n_t Src, n_t Dst) const override {
      return Problem.edgeTransfer(Src, Dst);
    }

    fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
      InterSummaryTransferEvaluator<AnalysisTypesT, K> Evaluator(Problem, ICF,
                                                                   Result, Ctx);
      return Evaluator.applyNormalEdge(T, In);
    }

    fact_t join(const fact_t &Lhs, const fact_t &Rhs) const override {
      return Problem.join(Lhs, Rhs);
    }

    bool equal(const fact_t &Lhs, const fact_t &Rhs) const override {
      return Problem.equal(Lhs, Rhs);
    }

    fact_t bottom() const override { return Problem.bottom(); }
    fact_t initialFact() const override { return EntryFact; }

  private:
    ProblemTy &Problem;
    f_t Function{};
    Context Ctx;
    const result_t &Result;
    const i_t &ICF;
    fact_t EntryFact;
  };

  void loadSeedFacts() { SeedFacts = Problem.initialSeeds(); }

  bool solveProcedureForContext(const ContextKey &Key, const i_t &ICF,
                                std::vector<ContextKey> &Frontier) {
    auto Function = Key.Inst != n_t{} ? ICF.getFunctionOf(Key.Inst) : f_t{};
    if (Function == f_t{} || ICF.getStartPointsOf(Function).empty()) {
      return false;
    }

    auto EntryFact = boundaryFactForContext(Function, Key.Ctx, ICF);
    ProcedureProblemAdapter Adapter(Problem, Function, Key.Ctx, Result, ICF,
                                    EntryFact);
    IntraEliminationSolver<AnalysisTypesT> Solver(Adapter);
    auto Status = Solver.solve();
    if (Status == SolveStatus::InvalidProblem) {
      return false;
    }

    bool Changed = false;
    const auto &ProcRes = Solver.getResults();
    const auto Nodes = Adapter.nodes();
    for (auto Inst : Nodes) {
      const auto *In = ProcRes.tryIN(Inst);
      if (In == nullptr) {
        continue;
      }
      auto &InSlot = Result.IN(Inst, Key.Ctx);
      if (!Problem.equal(InSlot, *In)) {
        InSlot = *In;
        Changed = true;
      }

      auto OutTransfer =
          Problem.direction() == dataflow::controlflow::FlowDirection::Backward
              ? Problem.edgeTransfer(n_t{}, Inst)
              : Problem.edgeTransfer(Inst, n_t{});
      auto Out = Problem.applyTransfer(OutTransfer, *In);
      auto &OutSlot = Result.OUT(Inst, Key.Ctx);
      if (!Problem.equal(OutSlot, Out)) {
        OutSlot = std::move(Out);
        Changed = true;
      }
    }

    if (Changed) {
      for (auto Inst : Nodes) {
        ContextKey ProcKey{Inst, Key.Ctx};
        auto Succs = successors(ProcKey, ICF);
        Frontier.insert(Frontier.end(), Succs.begin(), Succs.end());
      }
    }
    return Changed;
  }

  fact_t boundaryFactForContext(f_t Function, const Context &Ctx,
                                const i_t &ICF) {
    if (Function == f_t{}) {
      return Problem.bottom();
    }

    auto Starts = ICF.getStartPointsOf(Function);
    auto Exits = ICF.getExitPointsOf(Function);
    if (Starts.empty() && Exits.empty()) {
      return Problem.bottom();
    }

    auto EntryInst = Starts.empty() ? n_t{} : Starts.front();
    if (Problem.direction() ==
        ::dataflow::controlflow::FlowDirection::Backward) {
      fact_t Boundary = Problem.bottom();
      bool First = true;

      if (Ctx.empty()) {
        for (auto Exit : Exits) {
          auto It = SeedFacts.find(Exit);
          if (Exit == n_t{} || It == SeedFacts.end()) {
            continue;
          }
          if (First) {
            Boundary = It->second;
            First = false;
          } else {
            Boundary = Problem.join(Boundary, It->second);
          }
        }
        return First ? Problem.bottom() : Boundary;
      }

      auto CallerCtx = Ctx;
      auto CallSite = CallerCtx.pop_back();
      if (CallSite == n_t{}) {
        return Problem.bottom();
      }
      for (auto RetSite : ICF.getReturnSitesOfCallAt(CallSite)) {
        if (RetSite == n_t{}) {
          continue;
        }
        auto *RetFacts = Result.tryOUT(RetSite, CallerCtx);
        if (RetFacts == nullptr) {
          continue;
        }
        for (auto Exit : Exits) {
          if (Exit == n_t{}) {
            continue;
          }
          auto Flow =
              Problem.returnFlow(CallSite, Function, Exit, RetSite, *RetFacts);
          if (First) {
            Boundary = Flow;
            First = false;
          } else {
            Boundary = Problem.join(Boundary, Flow);
          }
        }
      }
      return First ? Problem.bottom() : Boundary;
    }

    if (Ctx.empty()) {
      auto It = SeedFacts.find(EntryInst);
      return It != SeedFacts.end() ? It->second : Problem.bottom();
    }

    auto CallerCtx = Ctx;
    auto CallSite = CallerCtx.pop_back();
    if (CallSite == n_t{}) {
      return Problem.bottom();
    }
    auto *CallerFacts = Result.tryOUT(CallSite, CallerCtx);
    if (CallerFacts == nullptr) {
      return Problem.bottom();
    }
    InterSummaryTransferEvaluator<AnalysisTypesT, K> Evaluator(
        Problem, ICF, Result, CallerCtx);
    return Evaluator.applyCallEntry(CallSite, Function, *CallerFacts);
  }

  std::vector<ContextKey> successors(const ContextKey &Key, const i_t &ICF) {
    std::vector<ContextKey> Next;
    auto Inst = Key.Inst;
    if (Inst == n_t{}) {
      return Next;
    }

    if (Problem.direction() ==
        ::dataflow::controlflow::FlowDirection::Backward) {
      for (auto Pred : ICF.getSuccsOf(
               Inst, dataflow::controlflow::FlowDirection::Forward)) {
        if (Pred != n_t{}) {
          Next.push_back({Pred, Key.Ctx});
        }
      }

      for (auto Pred : ICF.getPredsOf(
               Inst, dataflow::controlflow::FlowDirection::Forward)) {
        if (Pred == n_t{} || !ICF.isCallSite(Pred)) {
          continue;
        }
        for (auto RetSite : ICF.getReturnSitesOfCallAt(Pred)) {
          if (RetSite != Inst) {
            continue;
          }
          Context CalleeCtx = Key.Ctx;
          CalleeCtx.push_back(Pred);
          for (auto Callee : ICF.getCalleesOfCallAt(Pred)) {
            auto Starts = ICF.getStartPointsOf(Callee);
            auto Exits = ICF.getExitPointsOf(Callee);
            if (Starts.empty() || Exits.empty()) {
              continue;
            }
            Next.push_back({Starts.front(), CalleeCtx});
          }
          break;
        }
      }
      return Next;
    }

    for (auto Succ :
         ICF.getSuccsOf(Inst, dataflow::controlflow::FlowDirection::Forward)) {
      if (Succ != n_t{}) {
        Next.push_back({Succ, Key.Ctx});
      }
    }

    if (ICF.isExitInst(Inst)) {
      if (!Key.Ctx.empty()) {
        auto CallerCtx = Key.Ctx;
        auto CallSite = CallerCtx.pop_back();
        if (CallSite != n_t{}) {
          Next.push_back({CallSite, CallerCtx});
        }
        for (auto RetSite : ICF.getReturnSitesOfCallAt(CallSite)) {
          if (RetSite != n_t{}) {
            Next.push_back({RetSite, CallerCtx});
          }
        }
      } else if (K == 0) {
        auto Function = ICF.getFunctionOf(Inst);
        for (auto CallSite : ICF.getCallersOf(Function)) {
          for (auto RetSite : ICF.getReturnSitesOfCallAt(CallSite)) {
            if (RetSite != n_t{}) {
              Next.push_back({RetSite, Key.Ctx});
            }
          }
        }
      }
    }

    if (ICF.isCallSite(Inst)) {
      Context CalleeCtx = Key.Ctx;
      CalleeCtx.push_back(Inst);
      auto Callees = ICF.getCalleesOfCallAt(Inst);
      for (auto Callee : Callees) {
        auto Starts = ICF.getStartPointsOf(Callee);
        auto Exits = ICF.getExitPointsOf(Callee);
        if (Starts.empty() || Exits.empty()) {
          continue;
        }
        Next.push_back({Starts.front(), CalleeCtx});
      }
      for (auto RetSite : ICF.getReturnSitesOfCallAt(Inst)) {
        if (RetSite != n_t{}) {
          Next.push_back({RetSite, Key.Ctx});
        }
      }
    }
    return Next;
  }

  ProblemTy &Problem;
  result_t Result;
  bool HaveResult = false;
  SolveStatus LastStatus = SolveStatus::Ok;
  std::unordered_map<n_t, fact_t> SeedFacts;
};

} // namespace elimination

#endif // DATAFLOW_APA_SOLVER_INTERSOLVER_H_
