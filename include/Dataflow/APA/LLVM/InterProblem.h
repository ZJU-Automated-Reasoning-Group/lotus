#ifndef DATAFLOW_APA_LLVM_INTERPROBLEM_H_
#define DATAFLOW_APA_LLVM_INTERPROBLEM_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"

#include "Dataflow/APA/Core/InterProblem.h"

#include <algorithm>
#include <vector>

namespace elimination {

template <typename AnalysisDomainTy>
class LLVMInterEliminationProblem
    : public InterEliminationProblem<AnalysisDomainTy> {
public:
  using Base = InterEliminationProblem<AnalysisDomainTy>;
  using n_t = typename Base::n_t;
  using f_t = typename Base::f_t;
  using i_t = typename Base::i_t;

  enum class UnresolvedCallPolicy {
    Ignore,
    WarnAndIgnore,
  };

  explicit LLVMInterEliminationProblem(std::vector<f_t> EntryPoints = {},
                                       const i_t *ICF = nullptr)
      : Base(std::move(EntryPoints), ICF) {}

  std::vector<f_t> getCalleesOfCallAt(n_t CallSite) const override {
    std::vector<f_t> Callees;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr) {
      return Callees;
    }
    if (auto *Callee = Call->getCalledFunction()) {
      Callees.push_back(Callee);
    } else {
      Callees = resolve_indirect_callees(CallSite);
      if (Callees.empty() &&
          unresolved_call_policy() == UnresolvedCallPolicy::WarnAndIgnore) {
        llvm::errs()
            << "[InterEliminationProblem] WARNING: indirect call site "
               "encountered but no callee resolution was provided.\n"
            << "  Call site: " << *CallSite << "\n";
      }
    }
    return Callees;
  }

  virtual std::vector<f_t> resolve_indirect_callees(n_t CallSite) const {
    std::vector<f_t> Callees;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    auto *M = Call != nullptr ? Call->getModule() : nullptr;
    if (Call == nullptr || M == nullptr) {
      return Callees;
    }

    for (auto &F : *M) {
      if (F.isIntrinsic() || !has_compatible_signature(*Call, F)) {
        continue;
      }
      Callees.push_back(&F);
    }
    return Callees;
  }

  virtual UnresolvedCallPolicy unresolved_call_policy() const {
    return UnresolvedCallPolicy::WarnAndIgnore;
  }

protected:
  static bool has_compatible_signature(const llvm::CallBase &Call,
                                       const llvm::Function &Callee) {
    auto *CallTy = Call.getFunctionType();
    auto *CalleeTy = Callee.getFunctionType();

    if (CallTy == CalleeTy) {
      return true;
    }

    if (CallTy->getReturnType() != CalleeTy->getReturnType()) {
      return false;
    }

    const unsigned SharedParams =
        std::min(CallTy->getNumParams(), CalleeTy->getNumParams());
    for (unsigned I = 0; I < SharedParams; ++I) {
      if (CallTy->getParamType(I) != CalleeTy->getParamType(I)) {
        return false;
      }
    }

    if (CalleeTy->isVarArg()) {
      return CallTy->getNumParams() >= CalleeTy->getNumParams();
    }

    if (CallTy->isVarArg()) {
      return false;
    }

    return CallTy->getNumParams() == CalleeTy->getNumParams();
  }
};

} // namespace elimination

#endif // DATAFLOW_APA_LLVM_INTERPROBLEM_H_
