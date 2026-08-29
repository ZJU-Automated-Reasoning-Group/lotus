#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"

#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"
#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Analyses/Inter/UninitializedVariables.h"
#include "Dataflow/APA/Solver/ForwardInterSummarySolver.h"

#include <algorithm>
#include <iterator>
#include <memory>

namespace elimination {
namespace {

struct InterUninitializedVariablesAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = UninitVariablesFact;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

class InterElimUninitVariablesProblem
    : public LLVMInterEliminationProblem<InterUninitializedVariablesAnalysisTypes> {
public:
  explicit InterElimUninitVariablesProblem(
      llvm::Function *Entry, llvm::AAResults *AA = nullptr,
      llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
      const dataflow::controlflow::InterCFG *ICF = nullptr)
      : LLVMInterEliminationProblem<InterUninitializedVariablesAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF),
        DL(Entry != nullptr ? &Entry->getParent()->getDataLayout() : nullptr),
        AA(AA), AC(AC), DT(DT) {}

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      Out.insert(Alloca);
      return Out;
    }

    if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      auto *Val = Store->getValueOperand();
      if (Ptr == nullptr) {
        return Out;
      }
      if (llvm::isa<llvm::UndefValue>(Val) ||
          llvm::isa<llvm::PoisonValue>(Val)) {
        markAliasUninit(Out, Ptr);
        return Out;
      }
      if (llvm::isGuaranteedNotToBeUndefOrPoison(Val, AC, Store, DT)) {
        clearAliasUninit(Out, Ptr);
        clearAliasSetUninit(Out, Ptr);
        return Out;
      }

      auto *StoredInst = llvm::dyn_cast<llvm::Instruction>(Val);
      if (StoredInst != nullptr && In.count(StoredInst)) {
        markAliasUninit(Out, Ptr);
      } else {
        clearAliasUninit(Out, Ptr);
        clearAliasSetUninit(Out, Ptr);
      }
      return Out;
    }

    if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Load->getPointerOperand()))) {
        Out.insert(Load);
      }
      return Out;
    }

    if (auto *Bitcast = llvm::dyn_cast<llvm::BitCastInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Bitcast->getOperand(0)))) {
        Out.insert(Bitcast);
      }
      return Out;
    }

    if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Inst)) {
      if (In.count(normalizeMaybePointer(GEP->getPointerOperand()))) {
        Out.insert(GEP);
      }
      return Out;
    }

    if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      for (auto &IncomingUse : Phi->incoming_values()) {
        if (In.count(IncomingUse.get())) {
          Out.insert(Phi);
          break;
        }
      }
      return Out;
    }

    if (auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Select->getTrueValue())) ||
          In.count(normalizeMaybePointer(Select->getFalseValue()))) {
        Out.insert(Select);
      }
      return Out;
    }

    if (auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Cast->getOperand(0)))) {
        Out.insert(Cast);
      }
      return Out;
    }

    if (auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Inst)) {
      if (In.count(normalizeMaybePointer(Freeze->getOperand(0)))) {
        Out.insert(Freeze);
      }
      return Out;
    }

    if (auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      handleMemIntrinsics(Call, Out);
      return Out;
    }

    return Out;
  }

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return UninitializedVariablesDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return UninitializedVariablesDomain::equal(Lhs, Rhs);
  }

  fact_t allTop() const override {
    return UninitializedVariablesDomain::meetIdentity();
  }

  fact_t callFlow(n_t CallSite, f_t Callee, const fact_t &In) override {
    fact_t Out;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Callee == nullptr) {
      return Out;
    }

    llvm_inter::forEachActualFormalPair(
        Call, Callee,
        [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned /*Index*/) {
          if (In.count(Actual) ||
              (Actual != nullptr && Actual->getType()->isPointerTy() &&
               In.count(normalizeMaybePointer(Actual)))) {
            Out.insert(Formal);
          }
        });

    llvm_inter::copyGlobalValueFacts(In, Out);
    return Out;
  }

  fact_t returnFlow(n_t CallSite, f_t Callee, n_t ExitStmt, n_t /*RetSite*/,
                    const fact_t &In) override {
    fact_t Out;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    auto *Ret = llvm::dyn_cast_or_null<llvm::ReturnInst>(ExitStmt);
    if (Call == nullptr) {
      return Out;
    }

    llvm_inter::copyGlobalValueFacts(In, Out);

    if (Callee != nullptr) {
      llvm_inter::forEachActualFormalPair(
          Call, Callee,
          [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned /*Index*/) {
            if (Formal->getType()->isPointerTy() && In.count(Formal) &&
                Actual != nullptr) {
              Out.insert(normalizePointer(Actual));
            }
          });
    }

    if (llvm_inter::hasConcreteReturnValue(Call, Ret) &&
        In.count(Ret->getReturnValue())) {
      Out.insert(CallSite);
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

private:
  const llvm::DataLayout *DL = nullptr;
  llvm::AAResults *AA = nullptr;
  llvm::AssumptionCache *AC = nullptr;
  llvm::DominatorTree *DT = nullptr;

  const llvm::Value *getBaseObject(const llvm::Value *V) const {
    (void)DL;
    return llvm::getUnderlyingObject(V);
  }

  llvm::Value *normalizePointer(llvm::Value *Ptr) const {
    auto *Base = getBaseObject(Ptr);
    return Base != nullptr ? const_cast<llvm::Value *>(Base) : Ptr;
  }

  llvm::Value *normalizeMaybePointer(llvm::Value *V) const {
    if (V != nullptr && V->getType()->isPointerTy()) {
      return normalizePointer(V);
    }
    return V;
  }

  void clearAliasUninit(fact_t &Out, const llvm::Value *Ptr) const {
    auto *Base = getBaseObject(Ptr);
    auto *Norm = Base != nullptr ? Base : Ptr;
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Candidate = *It;
      if (Candidate == Norm) {
        It = Out.erase(It);
        continue;
      }
      if (Candidate->getType()->isPointerTy() &&
          getBaseObject(Candidate) == Base) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  void markAliasUninit(fact_t &Out, llvm::Value *Ptr) const {
    Out.insert(normalizePointer(Ptr));
  }

  void clearAliasSetUninit(fact_t &Out, llvm::Value *Ptr) const {
    if (AA == nullptr || Ptr == nullptr) {
      return;
    }
    llvm::MemoryLocation StoreLoc(
        Ptr, llvm::LocationSize::beforeOrAfterPointer(), llvm::AAMDNodes());
    for (auto It = Out.begin(); It != Out.end();) {
      auto *Candidate = *It;
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        ++It;
        continue;
      }
      llvm::MemoryLocation CandLoc(Candidate,
                                   llvm::LocationSize::beforeOrAfterPointer(),
                                   llvm::AAMDNodes());
      if (AA->alias(StoreLoc, CandLoc) != llvm::AliasResult::NoAlias) {
        It = Out.erase(It);
        continue;
      }
      ++It;
    }
  }

  static bool isMemIntrinsic(llvm::Function *Callee, llvm::Intrinsic::ID ID) {
    return Callee != nullptr && Callee->getIntrinsicID() == ID;
  }

  void handleMemIntrinsics(llvm::CallBase *Call, fact_t &Out) const {
    auto *Callee = Call->getCalledFunction();
    if (isMemIntrinsic(Callee, llvm::Intrinsic::memset)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Val = Call->getArgOperand(1);
        if (!llvm::isa<llvm::UndefValue>(Val)) {
          clearAliasUninit(Out, Dest);
        } else {
          markAliasUninit(Out, Dest);
        }
      }
      return;
    }
    if (isMemIntrinsic(Callee, llvm::Intrinsic::memcpy) ||
        isMemIntrinsic(Callee, llvm::Intrinsic::memmove)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Src = Call->getArgOperand(1);
        if (Out.count(Src)) {
          markAliasUninit(Out, Dest);
        } else {
          clearAliasUninit(Out, Dest);
        }
      }
    }
  }
};

} // namespace

InterUninitVariablesResult
runInterElimUninitVariables(llvm::Function *Entry, llvm::AAResults *AA,
                            llvm::AssumptionCache *AC, llvm::DominatorTree *DT,
                            const dataflow::controlflow::InterCFG *ICF) {
  InterUninitVariablesResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimUninitVariablesProblem Problem(Entry, AA, AC, DT, ICF);
  InterEliminationSolver<InterUninitializedVariablesAnalysisTypes,
                         kDefaultInterElimUninitVariablesCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

InterUninitVariablesResult runInterSummaryElimUninitVariables(
    llvm::Function *Entry, llvm::AAResults *AA, llvm::AssumptionCache *AC,
    llvm::DominatorTree *DT, const dataflow::controlflow::InterCFG *ICF,
    PathSummaryEquationOptions Options) {
  InterUninitVariablesResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimUninitVariablesProblem Problem(Entry, AA, AC, DT, ICF);
  ForwardInterSummarySolver<InterUninitializedVariablesAnalysisTypes,
                            kDefaultInterElimUninitVariablesCallStringLength>
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
