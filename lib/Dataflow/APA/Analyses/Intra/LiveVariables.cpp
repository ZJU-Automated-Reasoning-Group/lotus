#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include "Dataflow/APA/Analyses/Intra/LiveVariables.h"
#include "Dataflow/APA/Core/Problem.h"
#include "Dataflow/APA/LLVM/BackwardProblem.h"
#include "Dataflow/APA/Solver/Solver.h"
#include "Dataflow/ControlFlow/IntraCFG.h"

#include <unordered_set>

namespace elimination {
namespace {

using LiveVariablesAnalysisTypes = LLVMAnalysisTypes<LiveVariablesFact>;

class ReverseLiveVariablesProblem
    : public LLVMReverseIntraEliminationProblem<LiveVariablesFact> {
public:
  explicit ReverseLiveVariablesProblem(llvm::Function *F,
                                       llvm::Instruction *Exit)
      : LLVMReverseIntraEliminationProblem<LiveVariablesFact>(F, Exit) {}

  fact_t applyTransfer(const transfer_t &T, const fact_t &In) const override {
    auto *Inst = T;
    fact_t Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (llvm::isa<llvm::DbgInfoIntrinsic>(Inst)) {
      return Out;
    }

    if (!Inst->getType()->isVoidTy()) {
      Out.erase(Inst);
    }

    for (auto &Op : Inst->operands()) {
      auto *V = Op.get();
      if (llvm::isa<llvm::Instruction>(V) || llvm::isa<llvm::Argument>(V)) {
        Out.insert(V);
      }
    }

    return Out;
  }

  fact_t meet(const fact_t &Lhs, const fact_t &Rhs) const override {
    return LiveVariablesDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return LiveVariablesDomain::equal(Lhs, Rhs);
  }

  fact_t meetIdentity() const override {
    return LiveVariablesDomain::meetIdentity();
  }

  fact_t initialFact() const override { return fact_t{}; }
};

// Return the set of "real" exit instructions for backward analysis.
// We only include ReturnInst terminators; UnreachableInst and other
// no-successor terminators (resume, cleanupret, etc.) are excluded because
// propagating liveness facts backward from unreachable code is unsound —
// variables "used" on unreachable paths should not be considered live.
std::vector<llvm::Instruction *> getExitInstructions(llvm::Function *F) {
  std::vector<llvm::Instruction *> Exits;
  if (F == nullptr || F->isDeclaration()) {
    return Exits;
  }
  for (auto &BB : *F) {
    if (auto *Term = BB.getTerminator()) {
      if (llvm::isa<llvm::ReturnInst>(Term)) {
        Exits.push_back(Term);
      }
    }
  }
  return Exits;
}

} // namespace

LiveVariablesResult runIntraElimLiveVariables(llvm::Function *F,
                                              EliminationOptions Opts) {
  LiveVariablesResult Combined;
  if (F == nullptr || F->isDeclaration()) {
    return Combined;
  }

  auto Exits = getExitInstructions(F);
  if (Exits.empty()) {
    return Combined;
  }

  auto OverallStatus = SolveStatus::Ok;
  SolveDiagnostics OverallDiag;
  OverallDiag.requested_method = Opts.Method;
  OverallDiag.executed_method = Opts.Method;

  // Multi-exit handling: solve one reverse problem rooted at each return and
  // merge with set-union (may semantics: live on any feasible return path).
  for (auto *Exit : Exits) {
    ReverseLiveVariablesProblem Problem(F, Exit);
    IntraEliminationSolver<LiveVariablesAnalysisTypes> Solver(Problem, Opts);
    auto Status = Solver.solve();
    const auto &Diag = Solver.getDiagnostics();
    OverallDiag.used_adt = OverallDiag.used_adt || Diag.used_adt;
    OverallDiag.star_iterations_total += Diag.star_iterations_total;
    OverallDiag.max_star_hit = OverallDiag.max_star_hit || Diag.max_star_hit;
    if (Diag.fallback_reason != FallbackReason::None) {
      OverallDiag.fallback_reason = Diag.fallback_reason;
    }
    if (Status == SolveStatus::NonConvergentStar) {
      OverallStatus = SolveStatus::NonConvergentStar;
    } else if (Status == SolveStatus::InvalidProblem &&
               OverallStatus != SolveStatus::NonConvergentStar) {
      OverallStatus = SolveStatus::InvalidProblem;
    } else if (Status == SolveStatus::FallbackToState &&
               OverallStatus == SolveStatus::Ok) {
      OverallStatus = SolveStatus::FallbackToState;
    }
    auto Res = Solver.getResults();

    // The reverse adapter associates edge transfer with Dst. Re-apply transfer
    // at the synthetic exit root so combined facts model program-point liveness
    // after processing the exit instruction itself.
    const auto *ExitFacts = Res.tryIN(Exit);
    if (ExitFacts != nullptr) {
      Res.IN(Exit) = Problem.applyTransfer(Exit, *ExitFacts);
    }

    for (auto &BB : *F) {
      for (auto &I : BB) {
        auto *Inst = &I;
        auto &Out = Combined.IN(Inst);
        const auto *InFacts = Res.tryIN(Inst);
        if (InFacts == nullptr) {
          continue;
        }
        Out.insert(InFacts->begin(), InFacts->end());
      }
    }
  }

  Combined.setSolveMetadata(OverallStatus, OverallDiag);
  return Combined;
}

} // namespace elimination
