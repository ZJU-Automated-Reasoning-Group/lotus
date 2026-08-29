#include "Dataflow/APA/Analyses/Intra/Lockset.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Instructions.h"

namespace elimination {
namespace {

enum class LockAction { None, Lock, Unlock };

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

class ElimLocksetProblem : public LLVMIntraEliminationProblem<LocksetFact> {
public:
  explicit ElimLocksetProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<LocksetFact>(F) {}

  LocksetFact applyTransfer(const transfer_t &T,
                            const LocksetFact &In) const override {
    LocksetFact Out = In;
    const auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(T);
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

  LocksetFact meet(const LocksetFact &Lhs,
                   const LocksetFact &Rhs) const override {
    return LocksetDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const LocksetFact &Lhs, const LocksetFact &Rhs) const override {
    return LocksetDomain::equal(Lhs, Rhs);
  }

  LocksetFact meetIdentity() const override {
    return LocksetDomain::meetIdentity();
  }
  LocksetFact initialFact() const override { return LocksetFact{}; }
};

} // namespace

LocksetResult runIntraElimLockset(llvm::Function *F, EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return LocksetResult{};
  }

  ElimLocksetProblem Problem(F);
  IntraEliminationSolver<LLVMAnalysisTypes<LocksetFact>> Solver(Problem,
                                                                    Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
