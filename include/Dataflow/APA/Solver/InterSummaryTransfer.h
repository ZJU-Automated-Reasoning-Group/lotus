#ifndef DATAFLOW_APA_SOLVER_INTERSUMMARYTRANSFER_H_
#define DATAFLOW_APA_SOLVER_INTERSUMMARYTRANSFER_H_

#include "Dataflow/APA/Core/InterProblem.h"
#include "Dataflow/APA/Core/InterResult.h"
#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Core/PathExpr.h"
#include "Dataflow/ControlFlow/FlowDirection.h"
#include "Dataflow/Mono/Core/CallStringContext.h"

#include <cstddef>
#include <vector>

namespace elimination {

// First-class interprocedural transfer atom for summary-expression solving.
//
// Current APA interprocedural clients expose normalFlow/callFlow/returnFlow
// hooks. This atom type reifies those hooks so a solver can build and evaluate
// path expressions whose labels are interprocedural effects instead of only raw
// CFG edge transfers.
template <typename AnalysisTypesT> struct InterSummaryTransferAtom final {
  using n_t = typename AnalysisTypesT::n_t;
  using f_t = typename AnalysisTypesT::f_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;

  enum class Kind {
    RawNormal,
    Normal,
    CallEntry,
    ReturnExit,
    CallToRet,
  };

  static InterSummaryTransferAtom rawNormal(transfer_t Transfer) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::RawNormal;
    Atom.NormalTransfer = std::move(Transfer);
    return Atom;
  }

  static InterSummaryTransferAtom normal(transfer_t Transfer) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::Normal;
    Atom.NormalTransfer = std::move(Transfer);
    return Atom;
  }

  static InterSummaryTransferAtom callEntry(n_t CallSite, f_t Callee) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::CallEntry;
    Atom.CallSite = CallSite;
    Atom.Callee = Callee;
    return Atom;
  }

  static InterSummaryTransferAtom returnExit(n_t CallSite, f_t Callee,
                                             n_t ExitStmt, n_t RetSite) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::ReturnExit;
    Atom.CallSite = CallSite;
    Atom.Callee = Callee;
    Atom.ExitStmt = ExitStmt;
    Atom.RetSite = RetSite;
    return Atom;
  }

  static InterSummaryTransferAtom callToRet(n_t CallSite, n_t RetSite,
                                            std::vector<f_t> Callees) {
    InterSummaryTransferAtom Atom;
    Atom.K = Kind::CallToRet;
    Atom.CallSite = CallSite;
    Atom.RetSite = RetSite;
    Atom.Callees = std::move(Callees);
    return Atom;
  }

  Kind K = Kind::Normal;
  transfer_t NormalTransfer{};
  n_t CallSite{};
  f_t Callee{};
  n_t ExitStmt{};
  n_t RetSite{};
  std::vector<f_t> Callees;
};

template <typename AnalysisTypesT, unsigned K>
class InterSummaryTransferEvaluator final {
public:
  using ProblemTy = InterEliminationProblem<AnalysisTypesT>;
  using fact_t = typename AnalysisTypesT::fact_t;
  using n_t = typename AnalysisTypesT::n_t;
  using f_t = typename AnalysisTypesT::f_t;
  using transfer_t = typename AnalysisTypesT::transfer_t;
  using i_t = typename AnalysisTypesT::i_t;
  using result_t = InterDataFlowResultT<K, fact_t, transfer_t, n_t>;
  using Context = mono::CallStringCTX<n_t, K>;
  using atom_t = InterSummaryTransferAtom<AnalysisTypesT>;
  using expr_factory_t = PathExprFactory<atom_t>;
  using expr_ref_t = typename expr_factory_t::Ref;

  InterSummaryTransferEvaluator(ProblemTy &Problem, const i_t &ICF,
                                const result_t &Result, const Context &Ctx)
      : Problem(Problem), ICF(ICF), Result(Result), Ctx(Ctx) {}

  fact_t apply(const atom_t &Atom, const fact_t &In) const {
    switch (Atom.K) {
    case atom_t::Kind::RawNormal:
      return applyRawNormal(Atom.NormalTransfer, In);
    case atom_t::Kind::Normal:
      return applyNormalEdge(Atom.NormalTransfer, In);
    case atom_t::Kind::CallEntry:
      return applyCallEntry(Atom.CallSite, Atom.Callee, In);
    case atom_t::Kind::ReturnExit:
      return applyReturnExit(Atom.CallSite, Atom.Callee, Atom.ExitStmt,
                             Atom.RetSite, In);
    case atom_t::Kind::CallToRet:
      return applyCallToRet(Atom.CallSite, Atom.RetSite, Atom.Callees, In);
    }
    return Problem.bottom();
  }

  fact_t applyRawNormal(const transfer_t &T, const fact_t &In) const {
    return Problem.applyTransfer(T, In);
  }

  fact_t applyNormalEdge(const transfer_t &T, const fact_t &In) const {
    fact_t Out = Problem.applyTransfer(T, In);
    const auto Anchor = Problem.transferNode(T);
    if (!ICF.isCallSite(Anchor)) {
      return Out;
    }

    if (Problem.direction() != dataflow::controlflow::FlowDirection::Forward) {
      return Out;
    }

    const auto RetSite = Problem.transferSuccessor(T);
    return applyForwardCallEffects(Anchor, RetSite, In, Out);
  }

  fact_t applyCallEntry(n_t CallSite, f_t Callee, const fact_t &In) const {
    return Problem.callFlow(CallSite, Callee, In);
  }

  fact_t applyReturnExit(n_t CallSite, f_t Callee, n_t ExitStmt, n_t RetSite,
                         const fact_t &In) const {
    return Problem.returnFlow(CallSite, Callee, ExitStmt, RetSite, In);
  }

  fact_t applyReturnExitWithCallerFact(n_t CallSite, f_t Callee, n_t ExitStmt,
                                       n_t RetSite, const fact_t &CalleeExit,
                                       const fact_t &CallerFact) const {
    return Problem.returnFlowWithCallerFact(CallSite, Callee, ExitStmt, RetSite,
                                            CalleeExit, CallerFact);
  }

  fact_t applyCallToRet(n_t CallSite, n_t RetSite,
                        const std::vector<f_t> &Callees,
                        const fact_t &In) const {
    return Problem.callToRetFlow(CallSite, RetSite, Callees, In);
  }

  fact_t applyForwardCallEffects(n_t CallSite, n_t RetSite,
                                 const fact_t &BeforeCall,
                                 const fact_t &AfterNormal) const {
    fact_t Out = AfterNormal;
    if (RetSite != n_t{}) {
      Out = applyCallToRet(CallSite, RetSite, ICF.getCalleesOfCallAt(CallSite),
                           Out);
    }

    Context CalleeCtx = Ctx;
    CalleeCtx.push_back(CallSite);
    for (auto Callee : ICF.getCalleesOfCallAt(CallSite)) {
      auto Starts = ICF.getStartPointsOf(Callee);
      auto Exits = ICF.getExitPointsOf(Callee);
      if (Starts.empty() || Exits.empty()) {
        continue;
      }
      for (auto Exit : Exits) {
        if (Exit == n_t{}) {
          continue;
        }
        auto *ExitFacts = Result.tryOUT(Exit, CalleeCtx);
        if (ExitFacts == nullptr) {
          continue;
        }
        for (auto CandidateRetSite : ICF.getReturnSitesOfCallAt(CallSite)) {
          if (CandidateRetSite == n_t{}) {
            continue;
          }
          if (RetSite != n_t{} && CandidateRetSite != RetSite) {
            continue;
          }
          auto Returned = applyReturnExitWithCallerFact(
              CallSite, Callee, Exit, CandidateRetSite, *ExitFacts, BeforeCall);
          Out = Problem.join(Out, Returned);
        }
      }
    }
    return Out;
  }

  fact_t evaluateExpr(const expr_ref_t &Expr, const fact_t &In,
                      std::size_t MaxStarIterations = 100000) const {
    if (!Expr) {
      return Problem.bottom();
    }

    switch (Expr->K) {
    case expr_factory_t::Kind::Zero:
      return Problem.bottom();
    case expr_factory_t::Kind::One:
      return In;
    case expr_factory_t::Kind::Atom:
      return apply(*Expr->Transfer, In);
    case expr_factory_t::Kind::Union: {
      auto L = evaluateExpr(Expr->L, In, MaxStarIterations);
      auto R = evaluateExpr(Expr->R, In, MaxStarIterations);
      return Problem.join(L, R);
    }
    case expr_factory_t::Kind::Concat: {
      auto Mid = evaluateExpr(Expr->L, In, MaxStarIterations);
      return evaluateExpr(Expr->R, Mid, MaxStarIterations);
    }
    case expr_factory_t::Kind::Star: {
      auto Cur = In;
      for (std::size_t I = 0; I < MaxStarIterations; ++I) {
        auto Next =
            Problem.join(In, evaluateExpr(Expr->L, Cur, MaxStarIterations));
        if (Problem.equal(Next, Cur)) {
          return Cur;
        }
        Cur = std::move(Next);
      }
      return Cur;
    }
    }
    return Problem.bottom();
  }

private:
  ProblemTy &Problem;
  const i_t &ICF;
  const result_t &Result;
  Context Ctx;
};

} // namespace elimination

#endif // DATAFLOW_APA_SOLVER_INTERSUMMARYTRANSFER_H_
