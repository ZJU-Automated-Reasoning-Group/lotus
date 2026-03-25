#include "Dataflow/Mono/Analyses/Intra/IntraUninitVariables.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Dataflow/Mono/Container/Traits.h"
#include "Dataflow/Mono/Core/Domain.h"
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

#include <algorithm>
#include <memory>
#include <vector>

using namespace llvm;

namespace mono {
namespace {

using UninitVariablesDomain = LLVMMonoAnalysisDomain<SetContainer<Value *>>;

class UninitVariablesProblem : public IntraMonoProblem<UninitVariablesDomain> {
public:
  explicit UninitVariablesProblem(Function *F, lotus::AliasAnalysisWrapper *AA)
      : IntraMonoProblem<UninitVariablesDomain>({F}, AA),
        DL(&F->getParent()->getDataLayout()), AA(AA) {}

  mono_container_t allTop() override { return {}; }

  mono_container_t normalFlow(Instruction *Inst,
                              const mono_container_t &In) override {
    mono_container_t Out = In;

    if (auto *Alloca = dyn_cast<AllocaInst>(Inst)) {
      Out.insert(Alloca);
      return Out;
    }

    if (auto *Store = dyn_cast<StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      auto *Val = Store->getValueOperand();
      if (isa<UndefValue>(Val)) {
        markAliasUninit(Out, Ptr);
        return Out;
      }

      if (isUninitValue(Val, In)) {
        markAliasUninit(Out, Ptr);
      } else {
        clearAliasUninit(Out, Ptr);
      }
      return Out;
    }

    if (auto *Load = dyn_cast<LoadInst>(Inst)) {
      if (isUninitValue(Load->getPointerOperand(), In)) {
        Out.insert(Load);
      }
      return Out;
    }

    if (auto *Bitcast = dyn_cast<BitCastInst>(Inst)) {
      if (isUninitValue(Bitcast->getOperand(0), In)) {
        Out.insert(Bitcast);
      }
      return Out;
    }

    if (auto *GEP = dyn_cast<GetElementPtrInst>(Inst)) {
      if (isUninitValue(GEP->getPointerOperand(), In)) {
        Out.insert(GEP);
      }
      return Out;
    }

    if (auto *Phi = dyn_cast<PHINode>(Inst)) {
      for (auto &IncomingUse : Phi->incoming_values()) {
        auto *Incoming = IncomingUse.get();
        if (In.count(Incoming)) {
          Out.insert(Phi);
          break;
        }
      }
      return Out;
    }

    if (auto *Select = dyn_cast<SelectInst>(Inst)) {
      if (isUninitValue(Select->getTrueValue(), In) ||
          isUninitValue(Select->getFalseValue(), In)) {
        Out.insert(Select);
      }
      return Out;
    }

    if (auto *Call = dyn_cast<CallBase>(Inst)) {
      handleMemIntrinsics(Call, Out);
      return Out;
    }

    return Out;
  }

  // Uninitialized-variables is a FORWARD MAY-analysis: a variable is
  // considered uninitialized if it MIGHT be uninitialized on ANY path
  // reaching this point.  The join operator is therefore UNION, not
  // intersection.  Using intersection (must-analysis) would only flag
  // variables that are uninitialized on ALL paths, missing real bugs.
  mono_container_t merge(const mono_container_t &Lhs,
                         const mono_container_t &Rhs) override {
    mono_container_t Out = Lhs;
    Out.unionWith(Rhs);
    return Out;
  }

  bool equal_to(const mono_container_t &Lhs,
                const mono_container_t &Rhs) override {
    return Lhs == Rhs;
  }

  std::unordered_map<Instruction *, mono_container_t> initialSeeds() override {
    std::unordered_map<Instruction *, mono_container_t> Seeds;
    auto *F = getEntryPoints().empty() ? nullptr : getEntryPoints().front();
    if (F == nullptr || F->empty()) {
      return Seeds;
    }
    Seeds[&F->getEntryBlock().front()] = allTop();
    return Seeds;
  }

private:
  const DataLayout *DL;
  lotus::AliasAnalysisWrapper *AA;

  struct AliasPartition {
    std::vector<Value *> MustAliases;
    std::vector<Value *> MayAliases;
  };

  AliasPartition classifyAliases(const Value *Ptr,
                                 const mono_container_t &Facts) const {
    AliasPartition AP;
    if (Ptr == nullptr) {
      return AP;
    }
    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const Value *PtrBase = Ptr->stripPointerCasts();

    auto Add = [&](Value *Candidate) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        return;
      }
      const Value *CandidateBase = Candidate->stripPointerCasts();
      if (PtrBase != nullptr && CandidateBase != nullptr &&
          PtrBase == CandidateBase) {
        AP.MustAliases.push_back(Candidate);
        return;
      }
      if (Candidate == Ptr) {
        AP.MustAliases.push_back(Candidate);
        return;
      }
      if (!HaveAA || !Ptr->getType()->isPointerTy()) {
        AP.MayAliases.push_back(Candidate);
        return;
      }
      auto Res = AA->query(Ptr, Candidate);
      if (Res == AliasResult::MustAlias) {
        AP.MustAliases.push_back(Candidate);
      } else if (Res != AliasResult::NoAlias) {
        AP.MayAliases.push_back(Candidate);
      }
    };

    Add(const_cast<Value *>(Ptr));
    for (auto *V : Facts) {
      Add(V);
    }
    return AP;
  }

  const Value *getBaseObject(const Value *V) const {
    return llvm::getUnderlyingObject(V);
  }

  bool isUninitValue(const Value *V, const mono_container_t &In) const {
    if (V == nullptr) {
      return false;
    }
    if (In.count(const_cast<Value *>(V))) {
      return true;
    }
    if (!V->getType()->isPointerTy()) {
      return false;
    }
    const bool HaveAA = AA != nullptr && AA->isInitialized();
    const Value *VBase = V->stripPointerCasts();

    for (auto *Candidate : In) {
      if (Candidate == nullptr || !Candidate->getType()->isPointerTy()) {
        continue;
      }
      const Value *CandidateBase = Candidate->stripPointerCasts();
      if (VBase != nullptr && CandidateBase != nullptr &&
          VBase == CandidateBase) {
        return true;
      }
      if (!HaveAA) {
        return true;
      }
      if (AA->query(V, Candidate) != AliasResult::NoAlias) {
        return true;
      }
    }
    return false;
  }

  void clearAliasUninit(mono_container_t &Out, const Value *Ptr) const {
    if (AA != nullptr && AA->isInitialized() && Ptr != nullptr &&
        Ptr->getType()->isPointerTy()) {
      auto AP = classifyAliases(Ptr, Out);
      for (auto *Alias : AP.MustAliases) {
        Out.erase(Alias);
      }
      return;
    }
    auto *Base = getBaseObject(Ptr);
    // Collect elements to erase first, then erase them (SetContainer erase
    // returns bool)
    std::vector<Value *> ToErase;
    for (auto *Candidate : Out) {
      if (Candidate == Ptr) {
        ToErase.push_back(Candidate);
      } else if (Candidate->getType()->isPointerTy() &&
                 getBaseObject(Candidate) == Base) {
        ToErase.push_back(Candidate);
      }
    }
    for (auto *Elem : ToErase) {
      Out.erase(Elem);
    }
  }

  void markAliasUninit(mono_container_t &Out, Value *Ptr) const {
    auto AP = classifyAliases(Ptr, Out);
    for (auto *Alias : AP.MustAliases) {
      Out.insert(Alias);
    }
    for (auto *Alias : AP.MayAliases) {
      Out.insert(Alias);
    }
  }

  static bool isMemIntrinsic(Function *Callee, Intrinsic::ID ID) {
    return Callee != nullptr && Callee->getIntrinsicID() == ID;
  }

  void handleMemIntrinsics(CallBase *Call, mono_container_t &Out) {
    auto *Callee = Call->getCalledFunction();
    if (isMemIntrinsic(Callee, Intrinsic::memset)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Val = Call->getArgOperand(1);
        if (!isa<UndefValue>(Val)) {
          clearAliasUninit(Out, Dest);
        } else {
          markAliasUninit(Out, Dest);
        }
      }
      return;
    }
    if (isMemIntrinsic(Callee, Intrinsic::memcpy) ||
        isMemIntrinsic(Callee, Intrinsic::memmove)) {
      if (Call->arg_size() >= 2) {
        auto *Dest = Call->getArgOperand(0);
        auto *Src = Call->getArgOperand(1);
        if (isUninitValue(Src, Out)) {
          markAliasUninit(Out, Dest);
        } else {
          clearAliasUninit(Out, Dest);
        }
      }
      return;
    }
  }
};

} // namespace

std::unique_ptr<DataFlowResult> runIntraMonoUninitVariables(Function *F) {
  if (F == nullptr || F->isDeclaration()) {
    return nullptr;
  }

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *F->getParent(),
      lotus::AAConfig(lotus::AAConfig::Implementation::DyckAA,
                      lotus::AAConfig::ContextSensitivity::None, 0, true,
                      lotus::AAConfig::Solver::Default));
  UninitVariablesProblem Problem(F, AA.get());
  IntraMonoSolver<UninitVariablesDomain> Solver(Problem);
  Solver.solve();

  auto Result = std::make_unique<DataFlowResult>();
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      auto *I = &Inst;
      Result->IN(I) = Solver.getInResultsAt(I).getSet();
      Result->OUT(I) = Solver.getOutResultsAt(I).getSet();

      if (isa<AllocaInst>(I)) {
        Result->GEN(I).insert(I);
      }
      if (auto *Store = dyn_cast<StoreInst>(I)) {
        if (!isa<UndefValue>(Store->getValueOperand())) {
          Result->KILL(I).insert(Store->getPointerOperand());
        }
      }
    }
  }

  return Result;
}

} // namespace mono
