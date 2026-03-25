#include "Dataflow/APA/Clients/LLVM/Intra/Reachability.h"

namespace elimination {
namespace {

class ElimReachableProblem : public LLVMIntraEliminationProblem<ReachableFact> {
public:
  explicit ElimReachableProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<ReachableFact>(F) {}

  ReachableFact applyTransfer(const transfer_t & /*T*/,
                              const ReachableFact &In) const override {
    return In;
  }

  ReachableFact meet(const ReachableFact &Lhs,
                     const ReachableFact &Rhs) const override {
    return Lhs || Rhs;
  }

  bool equal_to(const ReachableFact &Lhs,
                const ReachableFact &Rhs) const override {
    return Lhs == Rhs;
  }

  ReachableFact meetIdentity() const override { return false; }

  ReachableFact initialFact() const override { return true; }
};

} // namespace

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachableResult{};
  }

  ElimReachableProblem Problem(F);
  IntraEliminationSolver<LLVMEliminationDomain<ReachableFact>> Solver(Problem,
                                                                      Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
