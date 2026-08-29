#include "Dataflow/APA/Analyses/Intra/ConstantPropagation.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueLattice.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

namespace elimination {
namespace {

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

bool isUnknown(const ConstantPropagationValue &V) { return V.isUnknown(); }

bool isOverdefined(const ConstantPropagationValue &V) {
  return V.isOverdefined();
}

bool isConst(const ConstantPropagationValue &V) { return V.isConstant(); }

bool isConstRange(const ConstantPropagationValue &V) {
  return V.isConstantRange();
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

bool isMemoryKey(const llvm::Value *V) {
  return V != nullptr && V->getType()->isPointerTy();
}

void clobberMemoryByCall(const llvm::CallBase *Call,
                         ConstantPropagationMap &Out, llvm::AAResults *AA) {
  if (Call == nullptr) {
    return;
  }
  if (AA == nullptr) {
    for (auto &Entry : Out) {
      if (isMemoryKey(Entry.first)) {
        Entry.second = makeOverdefined();
      }
    }
    return;
  }

  for (auto &Entry : Out) {
    const auto *Key = Entry.first;
    if (!isMemoryKey(Key)) {
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
  if (isUnknown(Lhs) || isUnknown(Rhs) || isOverdefined(Lhs) ||
      isOverdefined(Rhs)) {
    return makeOverdefined();
  }

  if (isConstRange(Lhs) && isConstRange(Rhs) && Inst != nullptr &&
      Inst->getType()->isIntegerTy()) {
    const auto &LR = Lhs.getConstantRange(true);
    const auto &RR = Rhs.getConstantRange(true);
    auto Op = static_cast<llvm::Instruction::BinaryOps>(Inst->getOpcode());
    llvm::ConstantRange Res =
        llvm::isa<llvm::OverflowingBinaryOperator>(Inst)
            ? LR.overflowingBinaryOp(
                  Op, RR,
                  (llvm::cast<llvm::OverflowingBinaryOperator>(Inst)
                           ->hasNoUnsignedWrap()
                       ? llvm::OverflowingBinaryOperator::NoUnsignedWrap
                       : 0u) |
                      (llvm::cast<llvm::OverflowingBinaryOperator>(Inst)
                               ->hasNoSignedWrap()
                           ? llvm::OverflowingBinaryOperator::NoSignedWrap
                           : 0u))
            : LR.binaryOp(Op, RR);
    if (Res.isSingleElement()) {
      return makeConst(
          llvm::ConstantInt::get(Inst->getType(), *Res.getSingleElement()));
    }
    return ConstantPropagationValue::getRange(Res, false);
  }

  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeOverdefined();
  }

  auto *L = Lhs.getConstant();
  auto *R = Rhs.getConstant();
  if (Inst->getType()->isVoidTy()) {
    return makeOverdefined();
  }
  if (auto *Folded = llvm::ConstantFoldBinaryOpOperands(
          Inst->getOpcode(), L, R, Inst->getModule()->getDataLayout())) {
    return makeConst(Folded);
  }
  return makeOverdefined();
}

ConstantPropagationValue evalICmp(const llvm::ICmpInst *ICmp,
                                  const ConstantPropagationValue &Lhs,
                                  const ConstantPropagationValue &Rhs) {
  if (ICmp == nullptr) {
    return makeOverdefined();
  }
  if (isUnknown(Lhs) || isUnknown(Rhs) || isOverdefined(Lhs) ||
      isOverdefined(Rhs)) {
    return makeOverdefined();
  }

  if (auto *C = Lhs.getCompare(ICmp->getPredicate(), ICmp->getType(), Rhs)) {
    if (llvm::isa<llvm::UndefValue>(C)) {
      return makeOverdefined();
    }
    return makeConst(C);
  }

  if (!isConst(Lhs) || !isConst(Rhs)) {
    return makeOverdefined();
  }

  auto *L = Lhs.getConstant();
  auto *R = Rhs.getConstant();
  auto *Folded = llvm::ConstantFoldCompareInstOperands(
      ICmp->getPredicate(), L, R, ICmp->getModule()->getDataLayout());
  if (auto *CI = llvm::dyn_cast_or_null<llvm::Constant>(Folded)) {
    return makeConst(CI);
  }
  return makeOverdefined();
}

ConstantPropagationValue evalSelect(const llvm::SelectInst *Select,
                                    const ConstantPropagationValue &Cond,
                                    const ConstantPropagationValue &TVal,
                                    const ConstantPropagationValue &FVal) {
  if (Select == nullptr) {
    return makeOverdefined();
  }
  if (auto C = Cond.asConstantInteger()) {
    return C->isZero() ? FVal : TVal;
  }
  if (Cond.isConstantRange(true) &&
      Select->getCondition()->getType()->isIntegerTy(1)) {
    const auto &CR = Cond.getConstantRange(true);
    llvm::APInt Zero(1, 0);
    llvm::APInt One(1, 1);
    if (!CR.contains(Zero)) {
      return TVal;
    }
    if (!CR.contains(One)) {
      return FVal;
    }
  }
  ConstantPropagationValue Out = TVal;
  Out.mergeIn(FVal);
  return Out;
}

ConstantPropagationValue evalPhi(const llvm::PHINode *Phi,
                                 const ConstantPropagationMap &In) {
  if (Phi == nullptr) {
    return makeOverdefined();
  }
  ConstantPropagationValue Acc = makeUnknown();
  for (const auto &Incoming : Phi->incoming_values()) {
    auto IncomingVal = resolveValue(In, Incoming.get());
    Acc.mergeIn(IncomingVal);
  }
  return Acc;
}

class ElimConstantPropagationProblem
    : public LLVMIntraEliminationProblem<ConstantPropagationMap, ConstantPropagationDomain> {
public:
  explicit ElimConstantPropagationProblem(
      llvm::Function *F, llvm::AAResults *AA = nullptr,
      llvm::AssumptionCache *AC = nullptr, llvm::DominatorTree *DT = nullptr,
      llvm::TargetLibraryInfo *TLI = nullptr)
      : LLVMIntraEliminationProblem<ConstantPropagationMap, ConstantPropagationDomain>(F),
        DL(F != nullptr ? &F->getParent()->getDataLayout() : nullptr), AA(AA),
        AC(AC), DT(DT), TLI(TLI) {}

  ConstantPropagationMap
  applyTransfer(const transfer_t &T,
                const ConstantPropagationMap &In) const override {
    auto *Inst = T;
    ConstantPropagationMap Out = In;
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
      if (Val.isConstant() || Val.isConstantRange()) {
        Out[Key] = Val;
      } else {
        Out[Key] = makeOverdefined();
      }
      if (AA != nullptr) {
        llvm::MemoryLocation StoreLoc = llvm::MemoryLocation::get(Store);
        for (auto &Entry : Out) {
          const auto *Cand = Entry.first;
          if (Cand == Key || Cand == nullptr ||
              !Cand->getType()->isPointerTy()) {
            continue;
          }
          llvm::MemoryLocation CandLoc(
              Cand, llvm::LocationSize::beforeOrAfterPointer(),
              llvm::AAMDNodes());
          if (AA->alias(StoreLoc, CandLoc) != llvm::AliasResult::NoAlias) {
            Entry.second = makeOverdefined();
          }
        }
      }
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
      auto Lhs = resolveValue(In, Op->getOperand(0));
      auto Rhs = resolveValue(In, Op->getOperand(1));
      Out[Op] = evalBinaryOp(Op, Lhs, Rhs);
      return Out;
    }

    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      auto Src = resolveValue(In, Cast->getOperand(0));
      if (Src.isConstantRange() && Cast->getType()->isIntegerTy()) {
        const auto &CR = Src.getConstantRange(true);
        const auto *DstTy = llvm::dyn_cast<llvm::IntegerType>(Cast->getType());
        if (DstTy != nullptr) {
          llvm::ConstantRange Res = CR;
          switch (Cast->getOpcode()) {
          case llvm::Instruction::ZExt:
            Res = CR.zextOrTrunc(DstTy->getBitWidth());
            break;
          case llvm::Instruction::SExt:
            Res = CR.sextOrTrunc(DstTy->getBitWidth());
            break;
          case llvm::Instruction::Trunc:
            Res = CR.truncate(DstTy->getBitWidth());
            break;
          default:
            Res = llvm::ConstantRange::getFull(DstTy->getBitWidth());
            break;
          }
          if (Res.isSingleElement()) {
            Out[Cast] = makeConst(llvm::ConstantInt::get(
                Cast->getType(), *Res.getSingleElement()));
          } else {
            Out[Cast] = ConstantPropagationValue::getRange(Res, false);
          }
          return Out;
        }
      }
      if (!isConst(Src)) {
        Out[Cast] = makeOverdefined();
        return Out;
      }
      auto *C = Src.getConstant();
      if (C == nullptr || DL == nullptr) {
        Out[Cast] = makeOverdefined();
        return Out;
      }
      if (auto *Folded =
              llvm::ConstantFoldUnaryOpOperand(Cast->getOpcode(), C, *DL)) {
        Out[Cast] = makeConst(Folded);
        return Out;
      }
      Out[Cast] = makeOverdefined();
      return Out;
    }

    if (const auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(Inst)) {
      auto Lhs = resolveValue(In, ICmp->getOperand(0));
      auto Rhs = resolveValue(In, ICmp->getOperand(1));
      Out[ICmp] = evalICmp(ICmp, Lhs, Rhs);
      return Out;
    }

    if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      auto Cond = resolveValue(In, Select->getCondition());
      auto TVal = resolveValue(In, Select->getTrueValue());
      auto FVal = resolveValue(In, Select->getFalseValue());
      auto Res = evalSelect(Select, Cond, TVal, FVal);
      Out[Select] = Res;
      return Out;
    }

    if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      Out[Phi] = evalPhi(Phi, In);
      return Out;
    }

    if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Inst)) {
      auto Val = resolveValue(In, Freeze->getOperand(0));
      Out[Freeze] =
          (Val.isConstant() || Val.isConstantRange()) ? Val : makeOverdefined();
      return Out;
    }

    if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      if (Call->mayWriteToMemory()) {
        clobberMemoryByCall(Call, Out, AA);
      }
    }

    if (DL != nullptr && !Inst->getType()->isVoidTy()) {
      llvm::SmallVector<llvm::Constant *, 4> Ops;
      Ops.reserve(Inst->getNumOperands());
      bool AllConst = true;
      for (unsigned I = 0; I < Inst->getNumOperands(); ++I) {
        const auto *OpV = Inst->getOperand(I);
        auto CV = resolveValue(In, OpV);
        if (!isConst(CV)) {
          AllConst = false;
          break;
        }
        auto *C = CV.getConstant();
        if (C == nullptr) {
          AllConst = false;
          break;
        }
        Ops.push_back(C);
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

  // meet is a symmetric join: for each key present in either map, merge the
  // two lattice values. Keys absent from one side are treated as Unknown
  // (bottom), so mergeIn(Unknown) leaves the other side unchanged — but we
  // must also handle keys present only in Lhs symmetrically.
  ConstantPropagationMap initialFact() const override {
    return ConstantPropagationMap{};
  }

private:
  const llvm::DataLayout *DL;
  llvm::AAResults *AA = nullptr;
  llvm::AssumptionCache *AC = nullptr;
  llvm::DominatorTree *DT = nullptr;
  llvm::TargetLibraryInfo *TLI = nullptr;
};

} // namespace

ConstantPropagationResult
runIntraElimConstantPropagation(llvm::Function *F, EliminationOptions Opts) {
  return runIntraElimConstantPropagation(F, nullptr, Opts);
}

ConstantPropagationResult
runIntraElimConstantPropagation(llvm::Function *F, llvm::AAResults *AA,
                                EliminationOptions Opts) {
  return runIntraElimConstantPropagation(F, AA, nullptr, nullptr, nullptr,
                                         Opts);
}

ConstantPropagationResult runIntraElimConstantPropagation(
    llvm::Function *F, llvm::AAResults *AA, llvm::AssumptionCache *AC,
    llvm::DominatorTree *DT, llvm::TargetLibraryInfo *TLI,
    EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return ConstantPropagationResult{};
  }

  ElimConstantPropagationProblem Problem(F, AA, AC, DT, TLI);
  IntraEliminationSolver<LLVMAnalysisTypes<ConstantPropagationMap, ConstantPropagationDomain>> Solver(
      Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
