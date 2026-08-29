#include "Dataflow/APA/Analyses/Inter/ConstantPropagation.h"

#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"

#include "Dataflow/APA/Analyses/Inter/FlowHelpers.h"
#include "Dataflow/APA/LLVM/InterProblem.h"
#include "Dataflow/APA/Solver/ForwardInterSummarySolver.h"

namespace elimination {
namespace {

struct InterConstantPropagationAnalysisTypes {
  using n_t = llvm::Instruction *;
  using fact_t = ConstantPropagationMap;
  using transfer_t = llvm::Instruction *;
  using f_t = llvm::Function *;
  using i_t = dataflow::controlflow::InterCFG;
};

ConstantPropagationValue makeUnknown() { return ConstantPropagationValue(); }

ConstantPropagationValue makeOverdefined() {
  return ConstantPropagationValue::getOverdefined();
}

ConstantPropagationValue makeConst(const llvm::Constant *Value) {
  if (Value == nullptr) {
    return makeOverdefined();
  }
  return ConstantPropagationValue::get(const_cast<llvm::Constant *>(Value));
}

const llvm::Value *getMemKey(const llvm::Value *Ptr) {
  auto *Base = llvm::getUnderlyingObject(Ptr);
  return Base != nullptr ? Base : Ptr;
}

ConstantPropagationValue resolveValue(const ConstantPropagationMap &In,
                                      const llvm::Value *V) {
  if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(V)) {
    if (GV->isConstant() && GV->hasInitializer()) {
      return makeConst(GV->getInitializer());
    }
  }
  if (auto *C = llvm::dyn_cast<llvm::Constant>(V)) {
    return makeConst(C);
  }

  auto It = In.find(V);
  if (It != In.end()) {
    return It->second;
  }
  return makeUnknown();
}

void clobberMemoryByCall(const llvm::CallBase *Call,
                         ConstantPropagationMap &Out, llvm::AAResults *AA) {
  if (Call == nullptr) {
    return;
  }
  if (AA == nullptr) {
    for (auto &Entry : Out) {
      if (Entry.first != nullptr && Entry.first->getType()->isPointerTy()) {
        Entry.second = makeOverdefined();
      }
    }
    return;
  }

  for (auto &Entry : Out) {
    const auto *Key = Entry.first;
    if (Key == nullptr || !Key->getType()->isPointerTy()) {
      continue;
    }
    llvm::MemoryLocation Loc(Key, llvm::LocationSize::beforeOrAfterPointer(),
                             llvm::AAMDNodes());
    if (llvm::isModSet(AA->getModRefInfo(Call, Loc))) {
      Entry.second = makeOverdefined();
    }
  }
}

ConstantPropagationValue evalBinaryOp(const llvm::Instruction *Inst,
                                      const ConstantPropagationValue &Lhs,
                                      const ConstantPropagationValue &Rhs) {
  if (Lhs.isUnknown() || Rhs.isUnknown() || Lhs.isOverdefined() ||
      Rhs.isOverdefined()) {
    return makeOverdefined();
  }
  if (!Lhs.isConstant() || !Rhs.isConstant()) {
    return makeOverdefined();
  }

  if (auto *LCI = llvm::dyn_cast<llvm::ConstantInt>(Lhs.getConstant())) {
    if (auto *RCI = llvm::dyn_cast<llvm::ConstantInt>(Rhs.getConstant())) {
      const auto &LA = LCI->getValue();
      const auto &RA = RCI->getValue();
      llvm::APInt Res(LA.getBitWidth(), 0);
      switch (Inst->getOpcode()) {
      case llvm::Instruction::Add:
        Res = LA + RA;
        break;
      case llvm::Instruction::Sub:
        Res = LA - RA;
        break;
      case llvm::Instruction::Mul:
        Res = LA * RA;
        break;
      case llvm::Instruction::SDiv:
      case llvm::Instruction::UDiv:
        if (RA.isZero()) {
          return makeOverdefined();
        }
        Res = Inst->getOpcode() == llvm::Instruction::SDiv ? LA.sdiv(RA)
                                                           : LA.udiv(RA);
        break;
      default:
        break;
      }
      if (Res.getBitWidth() == LA.getBitWidth()) {
        return makeConst(llvm::ConstantInt::get(Inst->getType(), Res));
      }
    }
  }

  auto *Folded = llvm::ConstantFoldBinaryOpOperands(
      Inst->getOpcode(), Lhs.getConstant(), Rhs.getConstant(),
      Inst->getModule()->getDataLayout());
  if (auto *C = llvm::dyn_cast_or_null<llvm::Constant>(Folded)) {
    return makeConst(C);
  }
  return makeOverdefined();
}

class InterElimConstantPropagationProblem
    : public LLVMInterEliminationProblem<InterConstantPropagationAnalysisTypes> {
public:
  explicit InterElimConstantPropagationProblem(
      llvm::Function *Entry, llvm::AAResults *AA = nullptr,
      llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
      llvm::TargetLibraryInfo *TLI = nullptr,
      const dataflow::controlflow::InterCFG *ICF = nullptr)
      : LLVMInterEliminationProblem<InterConstantPropagationAnalysisTypes>(
            std::vector<llvm::Function *>{Entry}, ICF),
        DL(Entry != nullptr ? &Entry->getParent()->getDataLayout() : nullptr),
        AA(AA), AC(AC), DT(DT), TLI(TLI) {}

  fact_t normalFlow(n_t Inst, const fact_t &In) override {
    fact_t Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = makeUnknown();
      }
      return Out;
    }

    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      if (Ptr == nullptr) {
        return Out;
      }
      auto Val = resolveValue(In, Store->getValueOperand());
      auto *Key = getMemKey(Ptr);
      Out[Key] =
          (Val.isConstant() || Val.isConstantRange()) ? Val : makeOverdefined();
      return Out;
    }

    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      auto *Key = getMemKey(Load->getPointerOperand());
      auto It = In.find(Key);
      if (It != In.end()) {
        Out[Load] = It->second;
      } else if (auto *GV = llvm::dyn_cast<llvm::GlobalVariable>(Key)) {
        Out[Load] = resolveValue(In, GV);
      }
      return Out;
    }

    if (const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(Inst)) {
      Out[Op] = evalBinaryOp(Op, resolveValue(In, Op->getOperand(0)),
                             resolveValue(In, Op->getOperand(1)));
      return Out;
    }

    if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      if (Call->mayWriteToMemory()) {
        clobberMemoryByCall(Call, Out, AA);
      }
    }

    if (DL != nullptr && !Inst->getType()->isVoidTy()) {
      llvm::SmallVector<llvm::Constant *, 4> Ops;
      bool AllConst = true;
      for (unsigned I = 0; I < Inst->getNumOperands(); ++I) {
        auto CV = resolveValue(In, Inst->getOperand(I));
        if (!CV.isConstant()) {
          AllConst = false;
          break;
        }
        Ops.push_back(CV.getConstant());
      }
      if (AllConst) {
        auto *Folded = llvm::ConstantFoldInstOperands(
            const_cast<llvm::Instruction *>(Inst), Ops, *DL);
        if (auto *C = llvm::dyn_cast_or_null<llvm::Constant>(Folded)) {
          Out[Inst] = makeConst(C);
          return Out;
        }
      }

      llvm::SimplifyQuery SQ(*DL, TLI, DT, AC, nullptr, true);
      SQ.CxtI = const_cast<llvm::Instruction *>(Inst);
      if (auto *Simplified = llvm::SimplifyInstruction(
              const_cast<llvm::Instruction *>(Inst), SQ)) {
        if (auto *C = llvm::dyn_cast<llvm::Constant>(Simplified)) {
          Out[Inst] = makeConst(C);
          return Out;
        }
      }
    }

    return Out;
  }

  fact_t merge(const fact_t &Lhs, const fact_t &Rhs) const override {
    return ConstantPropagationDomain::meet(Lhs, Rhs);
  }

  bool equal_to(const fact_t &Lhs, const fact_t &Rhs) const override {
    return ConstantPropagationDomain::equal(Lhs, Rhs);
  }

  fact_t allTop() const override {
    return ConstantPropagationDomain::meetIdentity();
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
          Out[Formal] = resolveValue(In, Actual);
          if (Formal->getType()->isPointerTy()) {
            auto *ActualKey = getMemKey(Actual);
            auto It = In.find(ActualKey);
            if (It != In.end()) {
              Out[Formal] = It->second;
            }
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
    if (Call == nullptr) {
      return Out;
    }

    auto *Callee = ExitStmt != nullptr ? ExitStmt->getFunction() : nullptr;
    if (Callee != nullptr) {
      llvm_inter::forEachActualFormalPair(
          Call, Callee,
          [&](llvm::Value *Actual, llvm::Argument *Formal, unsigned /*Index*/) {
            if (!Formal->getType()->isPointerTy()) {
              return;
            }
            auto It = In.find(Formal);
            if (It != In.end()) {
              Out[getMemKey(Actual)] = It->second;
            }
          });
    }

    if (Ret == nullptr || Call->getType()->isVoidTy()) {
      return Out;
    }

    auto *RetVal = Ret->getReturnValue();
    if (RetVal == nullptr) {
      return Out;
    }
    Out[CallSite] = resolveValue(In, RetVal);
    return Out;
  }

  fact_t callToRetFlow(n_t CallSite, n_t /*RetSite*/,
                       const std::vector<f_t> &Callees,
                       const fact_t &In) override {
    fact_t Out = In;
    auto *Call = llvm::dyn_cast_or_null<llvm::CallBase>(CallSite);
    if (Call == nullptr || Call->getType()->isVoidTy()) {
      return Out;
    }

    bool AllDefined = !Callees.empty();
    for (auto *Callee : Callees) {
      if (Callee == nullptr || Callee->isDeclaration() || Callee->empty()) {
        AllDefined = false;
        break;
      }
    }
    if (AllDefined) {
      // The bypass edge should not invent a value for the call result when the
      // callee body is available; returnFlow will contribute the precise fact.
      Out.erase(CallSite);
    } else {
      Out[CallSite] = makeOverdefined();
    }
    return Out;
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
  llvm::TargetLibraryInfo *TLI = nullptr;
};

} // namespace

InterConstantPropagationResult runInterElimConstantPropagation(
    llvm::Function *Entry, llvm::AAResults *AA, llvm::AssumptionCache *AC,
    llvm::DominatorTree *DT, llvm::TargetLibraryInfo *TLI,
    const dataflow::controlflow::InterCFG *ICF) {
  InterConstantPropagationResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimConstantPropagationProblem Problem(Entry, AA, AC, DT, TLI, ICF);
  InterEliminationSolver<InterConstantPropagationAnalysisTypes,
                         kDefaultInterElimConstantPropagationCallStringLength>
      Solver(Problem);
  auto Status = Solver.solve();
  if (const auto *Res = Solver.getResults()) {
    Out = *Res;
  }
  Out.setSolveStatus(Status);
  return Out;
}

InterConstantPropagationResult runInterSummaryElimConstantPropagation(
    llvm::Function *Entry, llvm::AAResults *AA, llvm::AssumptionCache *AC,
    llvm::DominatorTree *DT, llvm::TargetLibraryInfo *TLI,
    const dataflow::controlflow::InterCFG *ICF,
    PathSummaryEquationOptions Options) {
  InterConstantPropagationResult Out;
  if (Entry == nullptr || Entry->isDeclaration()) {
    return Out;
  }

  std::unique_ptr<dataflow::controlflow::LLVMInterCFG> OwnedICF;
  if (ICF == nullptr) {
    OwnedICF = std::make_unique<dataflow::controlflow::LLVMInterCFG>(
        Entry != nullptr ? Entry->getParent() : nullptr);
    ICF = OwnedICF.get();
  }

  InterElimConstantPropagationProblem Problem(Entry, AA, AC, DT, TLI, ICF);
  ForwardInterSummarySolver<
      InterConstantPropagationAnalysisTypes,
      kDefaultInterElimConstantPropagationCallStringLength>
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
