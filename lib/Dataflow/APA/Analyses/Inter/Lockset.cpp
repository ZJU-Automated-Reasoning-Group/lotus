#include "Dataflow/APA/Analyses/Inter/Lockset.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"
#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/ForwardInterSummarySolver.h"

namespace elimination {
namespace {

enum class LockAction { None, Lock, Unlock };

struct InterLocksetAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = LocksetFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

llvm::StringRef calledName(const llvm::CallBase *Call) {
  if (Call == nullptr) {
    return llvm::StringRef();
  }
  const auto *Callee = Call->getCalledFunction();
  return Callee != nullptr ? Callee->getName() : llvm::StringRef();
}

LockAction classifyCall(const llvm::CallBase *Call) {
  const auto Name = calledName(Call);
  if (Name.empty()) {
    return LockAction::None;
  }

  if (Name == "pthread_mutex_lock" || Name == "pthread_mutex_trylock" ||
      Name == "mtx_lock" || Name == "mtx_trylock" || Name == "mutex_lock" ||
      Name == "spin_lock" || Name == "raw_spin_lock") {
    return LockAction::Lock;
  }

  if (Name == "pthread_mutex_unlock" || Name == "mtx_unlock" ||
      Name == "mutex_unlock" || Name == "spin_unlock" ||
      Name == "raw_spin_unlock") {
    return LockAction::Unlock;
  }

  return LockAction::None;
}

const llvm::Value *lockKey(const llvm::Value *V) {
  if (V == nullptr) {
    return nullptr;
  }
  const auto *Base = llvm::getUnderlyingObject(V);
  return Base != nullptr ? Base : V;
}

const llvm::Value *lockOperand(const llvm::CallBase *Call) {
  if (Call == nullptr || Call->arg_empty()) {
    return nullptr;
  }
  return lockKey(Call->getArgOperand(0));
}

class InterElimLocksetProblem
    : public LLVMInterEliminationProblem<InterLocksetAnalysisTypes> {
public:
  explicit InterElimLocksetProblem(llvm::Function *Entry,
                                   const dataflow::controlflow::InterCFG *ICF)
      : LLVMInterEliminationProblem<InterLocksetAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF) {}

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    const auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(Inst);
    const auto Action = classifyCall(Call);
    if (Action == LockAction::None) {
      return Out;
    }

    const auto *Key = lockOperand(Call);
    if (Key == nullptr) {
      return Out;
    }

    if (Action == LockAction::Lock) {
      Out.insert(Key);
    } else {
      Out.erase(Key);
    }
    return Out;
  }

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return LocksetDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return LocksetDomain::equal(Lhs, Rhs);
  }

  fact_t allTop() const override { return LocksetDomain::meetIdentity(); }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    fact_t Out;
    const auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr) {
      return Out;
    }

    llvm_inter::copyGlobalValueFacts(In, Out);
    llvm_inter::forEachActualFormalPair(
        Call, Callee,
        [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned /*Index*/) {
          if (Actual != nullptr && In.count(lockKey(Actual))) {
            Out.insert(Formal);
          }
        });
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t Callee, n_t /*ExitStmt*/, n_t /*RetSite*/,
                    const fact_t &In) override {
    fact_t Out;
    const auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr) {
      return Out;
    }

    llvm_inter::copyGlobalValueFacts(In, Out);
    if (Callee != nullptr) {
      llvm_inter::forEachActualFormalPair(
          Call, Callee,
          [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned /*Index*/) {
            if (Actual != nullptr && In.count(Formal)) {
              Out.insert(lockKey(Actual));
            }
          });
    }
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t /*RetSite*/,
                       const std::vector<f_t> & /*Callees*/,
                       const fact_t &In) override {
    return normalFlow(CallSite, In);
  }

  std::unordered_map<n_t, fact_t> initialSeeds() override {
    std::unordered_map<n_t, fact_t> Seeds;
    auto *Entry = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (Entry == nullptr || Entry->empty()) {
      return Seeds;
    }
    Seeds[&*Entry->getEntryBlock().begin()] = fact_t{};
    return Seeds;
  }
};

} // namespace

InterLocksetResult
runInterElimLockset(llvm::Function *Entry,
                    const dataflow::controlflow::InterCFG *ICF) {
  InterLocksetResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimLocksetProblem Problem(Entry, ICF);
  InterEliminationSolver<InterLocksetAnalysisTypes,
                         kDefaultInterElimLocksetCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

InterLocksetResult
runInterSummaryElimLockset(llvm::Function *Entry,
                           const dataflow::controlflow::InterCFG *ICF,
                           PathSummaryEquationOptions Options) {
  InterLocksetResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimLocksetProblem Problem(Entry, ICF);
  ForwardInterSummarySolver<InterLocksetAnalysisTypes,
                            kDefaultInterElimLocksetCallStringLength>
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
