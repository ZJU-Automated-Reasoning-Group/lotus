#include "Dataflow/APA/Analyses/Inter/LiveVariables.h"

#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"
#include "Dataflow/APA/LLVM/InterProblem.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

namespace elimination {
namespace {

struct InterLiveVariablesAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = LiveVariablesFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

class InterElimLiveVariablesProblem
    : public LLVMInterEliminationProblem<InterLiveVariablesAnalysisTypes> {
public:
  explicit InterElimLiveVariablesProblem(
      llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterLiveVariablesAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF) {}

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    if (Inst == nullptr || llvm::isa<llvm::DbgInfoIntrinsic>(Inst)) {
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

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return LiveVariablesDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return LiveVariablesDomain::equal(Lhs, Rhs);
  }

  fact_t allTop() const override { return LiveVariablesDomain::meetIdentity(); }

  ::dataflow::controlflow::FlowDirection direction() const override {
    return ::dataflow::controlflow::FlowDirection::Backward;
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    fact_t Out;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr) {
      return Out;
    }

    llvm_inter::forEachActualFormalPair(Call, Callee,
                                        [&](llvm::Value *Actual,
                                            llvm::Argument *Formal,
                                            unsigned /*Index*/) {
                                          if (In.count(Formal)) {
                                            Out.insert(Actual);
                                          }
                                        });

    llvm_inter::copyGlobalValueFacts(In, Out);
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t /*Callee*/, n_t ExitStmt, n_t /*RetSite*/,
                    const fact_t &In) override {
    fact_t Out;
    llvm_inter::copyGlobalValueFacts(In, Out);

    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Ret == nullptr || Call == nullptr) {
      return Out;
    }
    if (auto *RetVal = Ret->getReturnValue()) {
      if (In.count(CallSite) && (llvm::isa<llvm::Instruction>(RetVal) ||
                                 llvm::isa<llvm::Argument>(RetVal))) {
        Out.insert(RetVal);
      }
    }
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t /*RetSite*/,
                       const std::vector<f_t> & /*Callees*/,
                       const fact_t &In) override {
    fact_t Out = In;
    if (CallSite != nullptr && !CallSite->getType()->isVoidTy()) {
      Out.erase(CallSite);
    }
    for (auto &Op : CallSite->operands()) {
      auto *V = Op.get();
      if (llvm::isa<llvm::Instruction>(V) || llvm::isa<llvm::Argument>(V)) {
        Out.insert(V);
      }
    }
    return Out;
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr) {
      return Seeds;
    }
    for (auto &BB : *Entry) {
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
        Seeds[Ret] = {};
      }
    }
    return Seeds;
  }
};

} // namespace

InterLiveVariablesResult runInterElimLiveVariables(
    llvm::Function *Entry, const dataflow::controlflow::InterCFG *ICF) {
  InterLiveVariablesResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimLiveVariablesProblem Problem(Entry, ICF);
  InterEliminationSolver<InterLiveVariablesAnalysisTypes,
                         kDefaultInterElimLiveVariablesCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

} // namespace elimination
