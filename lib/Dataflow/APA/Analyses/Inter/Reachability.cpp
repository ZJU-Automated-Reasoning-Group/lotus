#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Analyses/Inter/Reachability.h"
#include "Dataflow/APA/Solver/ForwardInterSummarySolver.h"

namespace elimination {
namespace {

struct InterReachabilityAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = ReachableFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

class InterElimReachableProblem
    : public LLVMInterEliminationProblem<InterReachabilityAnalysisTypes> {
public:
  explicit InterElimReachableProblem(llvm::Function *Entry,
                                     const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterReachabilityAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF) {}

  fact_t normalFlow(n_t /*Inst*/, const fact_t &In) override { return In; }

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return ReachabilityDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return ReachabilityDomain::equal(Lhs, Rhs);
  }

  fact_t allTop() const override { return ReachabilityDomain::meetIdentity(); }

  fact_t callFlow(n_t /*CallSite*/, f_t /*Callee*/, const fact_t &In) override {
    return In;
  }

  fact_t returnFlow(n_t /*CallSite*/, f_t /*Callee*/, n_t /*ExitStmt*/,
                    n_t /*RetSite*/, const fact_t &In) override {
    return In;
  }

  fact_t callToRetFlow(n_t /*CallSite*/, n_t /*RetSite*/,
                       const std::vector<f_t> & /*Callees*/,
                       const fact_t &In) override {
    return In;
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr || Entry->empty()) {
      return Seeds;
    }
    Seeds[&*Entry->getEntryBlock().begin()] = true;
    return Seeds;
  }
};

} // namespace

InterReachableResult
runInterElimReachable(llvm::Function *Entry,
                      const dataflow::controlflow::InterCFG *ICF) {
  InterReachableResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimReachableProblem Problem(Entry, ICF);
  InterEliminationSolver<InterReachabilityAnalysisTypes,
                         kDefaultInterElimReachabilityCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

InterReachableResult
runInterSummaryElimReachable(llvm::Function *Entry,
                             const dataflow::controlflow::InterCFG *ICF,
                             PathSummaryEquationOptions Options) {
  InterReachableResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimReachableProblem Problem(Entry, ICF);
  ForwardInterSummarySolver<InterReachabilityAnalysisTypes,
                            kDefaultInterElimReachabilityCallStringLength>
      Solver(Problem, Options);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  Out.setSummarySolveDiagnostics(Solver.resultDiagnostics());
  return Out;
}

} // namespace elimination
