#ifndef DATAFLOW_APA_ENGINES_SOLVER_H_
#define DATAFLOW_APA_ENGINES_SOLVER_H_

#include "Dataflow/APA/Core/Options.h"
#include "Dataflow/APA/Solver/ADTDelayedSolver.h"
#include "Dataflow/APA/Solver/ADTSimpleSolver.h"
#include "Dataflow/APA/Solver/StateEliminationSolver.h"

namespace elimination {

// Public entry point for the intraprocedural APA solver.
//
// This class intentionally stays thin: it owns the shared solver context and
// dispatches to one of the three engine implementations based on
// EliminationOptions. The heavy algorithmic logic lives in the engine headers
// so that each solver family can be read and maintained independently.
template <typename AnalysisTypesT> class IntraEliminationSolver final {
public:
  using Context = detail::IntraEliminationSolverContext<AnalysisTypesT>;
  using ProblemTy = typename Context::ProblemTy;
  using ReducibleProblemTy = typename Context::ReducibleProblemTy;
  using n_t = typename Context::n_t;
  using fact_t = typename Context::fact_t;
  using transfer_t = typename Context::transfer_t;
  using expr_factory_t = typename Context::expr_factory_t;
  using expr_ref_t = typename Context::expr_ref_t;
  using result_t = typename Context::result_t;

  explicit IntraEliminationSolver(const ProblemTy &Problem,
                                  EliminationOptions Opts = {})
      : Ctx(Problem, Opts), Opts(Opts) {}

  // Try the requested engine first. ADT-based methods may reject the problem if
  // reducibility assumptions do not hold; in that case we transparently fall
  // back to the generic state-elimination engine.
  SolveStatus solve() {
    UsedADT = false;
    LastStatus = SolveStatus::Ok;
    Ctx.Diagnostics = {};
    Ctx.Diagnostics.requested_method = Opts.Method;
    Ctx.Diagnostics.executed_method = EliminationMethod::StateElimination;

    if (Opts.Method == EliminationMethod::ADTSimple) {
      if (detail::solveADTSimple(Ctx)) {
        UsedADT = true;
        Ctx.Diagnostics.used_adt = true;
        Ctx.Diagnostics.executed_method = EliminationMethod::ADTSimple;
        LastStatus = Ctx.StarNonConvergent ? SolveStatus::NonConvergentStar
                                           : SolveStatus::Ok;
        return LastStatus;
      }
      Ctx.Diagnostics.fallback_reason = FallbackReason::ADTRejected;
    }
    if (Opts.Method == EliminationMethod::ADTDelayed) {
      if (detail::solveADTDelayed(Ctx)) {
        UsedADT = true;
        Ctx.Diagnostics.used_adt = true;
        Ctx.Diagnostics.executed_method = EliminationMethod::ADTDelayed;
        LastStatus = Ctx.StarNonConvergent ? SolveStatus::NonConvergentStar
                                           : SolveStatus::Ok;
        return LastStatus;
      }
      Ctx.Diagnostics.fallback_reason = FallbackReason::ADTRejected;
    }
    Ctx.Diagnostics.executed_method = EliminationMethod::StateElimination;
    if ((Opts.Method == EliminationMethod::ADTSimple ||
         Opts.Method == EliminationMethod::ADTDelayed) &&
        Ctx.Diagnostics.fallback_reason == FallbackReason::None) {
      Ctx.Diagnostics.fallback_reason = FallbackReason::ADTRejected;
    }
    if (!detail::solveStateElimination(Ctx)) {
      Ctx.Diagnostics.fallback_reason = FallbackReason::InvalidProblem;
      LastStatus = SolveStatus::InvalidProblem;
      return LastStatus;
    }
    if (Ctx.StarNonConvergent) {
      LastStatus = SolveStatus::NonConvergentStar;
      return LastStatus;
    }
    LastStatus =
        (Ctx.Diagnostics.fallback_reason == FallbackReason::ADTRejected)
            ? SolveStatus::FallbackToState
            : SolveStatus::Ok;
    return LastStatus;
  }

  const result_t &getResults() const { return Ctx.Results; }
  SolveStatus getLastStatus() const { return LastStatus; }
  const SolveDiagnostics &getDiagnostics() const { return Ctx.Diagnostics; }
  bool usedADT() const { return UsedADT; }

private:
  Context Ctx;
  EliminationOptions Opts;
  SolveStatus LastStatus = SolveStatus::Ok;
  bool UsedADT = false;
};

} // namespace elimination

#endif // DATAFLOW_APA_ENGINES_SOLVER_H_
