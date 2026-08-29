#include "Dataflow/APA/Analyses/Intra/Reachability.h"

namespace elimination {
namespace {

class ElimReachableProblem : public LLVMIntraEliminationProblem<ReachableFact, ReachabilityDomain> {
public:
  explicit ElimReachableProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<ReachableFact, ReachabilityDomain>(F) {}

  ReachableFact applyTransfer(const transfer_t & /*T*/,
                              const ReachableFact &In) const override {
    return In;
  }

  ReachableFact initialFact() const override { return true; }
};

} // namespace

ReachableResult runIntraElimReachable(llvm::Function *F,
                                      EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ReachableResult{};
  }

  ElimReachableProblem Problem(F);
  IntraEliminationSolver<LLVMAnalysisTypes<ReachableFact, ReachabilityDomain>> Solver(Problem,
                                                                      Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
