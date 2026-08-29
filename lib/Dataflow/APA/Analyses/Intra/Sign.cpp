#include "Dataflow/APA/Analyses/Intra/Sign.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"

namespace elimination {
namespace {

bool isIntegerLike(const llvm::Value *V) {
  return V != nullptr && V->getType()->isIntegerTy();
}

SignValue signOfConstant(const llvm::Constant *C) {
  if (const auto *CI = llvm::dyn_cast_or_null<llvm::ConstantInt>(C)) {
    if (CI->isZero()) {
      return SignValue::zero();
    }
    return CI->isNegative() ? SignValue::negative() : SignValue::positive();
  }
  return SignValue::top();
}

const llvm::Value *getMemKey(const llvm::Value *Ptr) {
  auto *Base = llvm::getUnderlyingObject(Ptr);
  return Base != nullptr ? Base : Ptr;
}

SignValue resolveValue(const SignMap &In, const llvm::Value *V) {
  if (auto *GV = llvm::dyn_cast_or_null<llvm::GlobalVariable>(V)) {
    if (GV->isConstant() && GV->hasInitializer() &&
        GV->getInitializer()->getType()->isIntegerTy()) {
      return signOfConstant(GV->getInitializer());
    }
  }
  if (auto *C = llvm::dyn_cast_or_null<llvm::Constant>(V)) {
    return signOfConstant(C);
  }
  auto It = In.find(V);
  return It != In.end() ? It->second : SignValue::bottom();
}

SignValue negate(SignValue V) {
  std::uint8_t Mask = SignValue::None;
  if (V.mayBeNegative()) {
    Mask |= SignValue::Positive;
  }
  if (V.mayBeZero()) {
    Mask |= SignValue::Zero;
  }
  if (V.mayBePositive()) {
    Mask |= SignValue::Negative;
  }
  return SignValue(Mask);
}

SignValue addSigns(SignValue L, SignValue R) {
  std::uint8_t Mask = SignValue::None;
  auto AddCase = [&](SignValue A, SignValue B, SignValue Res) {
    if ((L.bits() & A.bits()) != 0 && (R.bits() & B.bits()) != 0) {
      Mask |= Res.bits();
    }
  };
  AddCase(SignValue::negative(), SignValue::negative(), SignValue::negative());
  AddCase(SignValue::negative(), SignValue::zero(), SignValue::negative());
  AddCase(SignValue::zero(), SignValue::negative(), SignValue::negative());
  AddCase(SignValue::zero(), SignValue::zero(), SignValue::zero());
  AddCase(SignValue::zero(), SignValue::positive(), SignValue::positive());
  AddCase(SignValue::positive(), SignValue::zero(), SignValue::positive());
  AddCase(SignValue::positive(), SignValue::positive(), SignValue::positive());
  AddCase(SignValue::negative(), SignValue::positive(), SignValue::top());
  AddCase(SignValue::positive(), SignValue::negative(), SignValue::top());
  return SignValue(Mask);
}

SignValue mulSigns(SignValue L, SignValue R) {
  std::uint8_t Mask = SignValue::None;
  if (L.mayBeZero() || R.mayBeZero()) {
    Mask |= SignValue::Zero;
  }
  if ((L.mayBeNegative() && R.mayBeNegative()) ||
      (L.mayBePositive() && R.mayBePositive())) {
    Mask |= SignValue::Positive;
  }
  if ((L.mayBeNegative() && R.mayBePositive()) ||
      (L.mayBePositive() && R.mayBeNegative())) {
    Mask |= SignValue::Negative;
  }
  return SignValue(Mask);
}

SignValue divSigns(SignValue L, SignValue R) {
  if (R.mayBeZero()) {
    return SignValue::top();
  }
  return mulSigns(L, R);
}

SignValue evalBinaryOp(const llvm::BinaryOperator *Op, SignValue L,
                       SignValue R) {
  if (Op == nullptr || L.isBottom() || R.isBottom()) {
    return SignValue::bottom();
  }
  switch (Op->getOpcode()) {
  case llvm::Instruction::Add:
    return addSigns(L, R);
  case llvm::Instruction::Sub:
    return addSigns(L, negate(R));
  case llvm::Instruction::Mul:
    return mulSigns(L, R);
  case llvm::Instruction::SDiv:
  case llvm::Instruction::SRem:
    return divSigns(L, R);
  case llvm::Instruction::UDiv:
  case llvm::Instruction::URem:
  case llvm::Instruction::LShr:
    return SignValue::nonNegative();
  case llvm::Instruction::And:
    if (L == SignValue::zero() || R == SignValue::zero()) {
      return SignValue::zero();
    }
    return SignValue::top();
  case llvm::Instruction::Or:
  case llvm::Instruction::Xor:
    if (L == SignValue::zero()) {
      return R;
    }
    if (R == SignValue::zero()) {
      return L;
    }
    return SignValue::top();
  default:
    return SignValue::top();
  }
}

SignValue evalCast(const llvm::CastInst *Cast, SignValue Src) {
  if (Cast == nullptr || Src.isBottom() || !Cast->getType()->isIntegerTy()) {
    return SignValue::bottom();
  }
  switch (Cast->getOpcode()) {
  case llvm::Instruction::ZExt:
  case llvm::Instruction::PtrToInt:
    return SignValue::nonNegative();
  case llvm::Instruction::SExt:
    return Src;
  case llvm::Instruction::Trunc:
    return Cast->getType()->isIntegerTy(1) ? SignValue::nonNegative()
                                          : SignValue::top();
  default:
    return SignValue::top();
  }
}

SignValue evalSelect(const llvm::SelectInst *Select, const SignMap &In) {
  auto Cond = resolveValue(In, Select->getCondition());
  auto TrueVal = resolveValue(In, Select->getTrueValue());
  auto FalseVal = resolveValue(In, Select->getFalseValue());
  if (Cond == SignValue::zero()) {
    return FalseVal;
  }
  if (!Cond.mayBeZero() && !Cond.isBottom()) {
    return TrueVal;
  }
  TrueVal.mergeIn(FalseVal);
  return TrueVal;
}

SignValue evalPhi(const llvm::PHINode *Phi, const SignMap &In) {
  SignValue Out = SignValue::bottom();
  for (const auto &Incoming : Phi->incoming_values()) {
    Out.mergeIn(resolveValue(In, Incoming.get()));
  }
  return Out;
}

void clobberMemoryByCall(const llvm::CallBase *Call, SignMap &Out) {
  if (Call == nullptr || !Call->mayWriteToMemory()) {
    return;
  }
  for (auto &Entry : Out) {
    if (Entry.first != nullptr && Entry.first->getType()->isPointerTy()) {
      Entry.second = SignValue::top();
    }
  }
}

class ElimSignAnalysisProblem : public LLVMIntraEliminationProblem<SignMap, SignDomain> {
public:
  explicit ElimSignAnalysisProblem(llvm::Function *F)
      : LLVMIntraEliminationProblem<SignMap, SignDomain>(F) {}

  SignMap applyTransfer(const transfer_t &T, const SignMap &In) const override {
    auto *Inst = T;
    SignMap Out = In;
    if (Inst == nullptr) {
      return Out;
    }

    if (const auto *Alloca = llvm::dyn_cast<llvm::AllocaInst>(Inst)) {
      if (Alloca->getAllocatedType()->isIntegerTy()) {
        Out[Alloca] = SignValue::bottom();
      }
      return Out;
    }

    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(Inst)) {
      auto *Ptr = Store->getPointerOperand();
      if (Ptr != nullptr && Store->getValueOperand()->getType()->isIntegerTy()) {
        Out[getMemKey(Ptr)] = resolveValue(In, Store->getValueOperand());
      }
      return Out;
    }

    if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(Inst)) {
      if (Load->getType()->isIntegerTy()) {
        auto *Key = getMemKey(Load->getPointerOperand());
        auto It = In.find(Key);
        Out[Load] = It != In.end() ? It->second : SignValue::top();
      }
      return Out;
    }

    if (const auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(Inst)) {
      if (Op->getType()->isIntegerTy()) {
        Out[Op] = evalBinaryOp(Op, resolveValue(In, Op->getOperand(0)),
                               resolveValue(In, Op->getOperand(1)));
      }
      return Out;
    }

    if (const auto *Cast = llvm::dyn_cast<llvm::CastInst>(Inst)) {
      if (Cast->getType()->isIntegerTy()) {
        Out[Cast] = evalCast(Cast, resolveValue(In, Cast->getOperand(0)));
      }
      return Out;
    }

    if (const auto *ICmp = llvm::dyn_cast<llvm::ICmpInst>(Inst)) {
      Out[ICmp] = SignValue::nonNegative();
      return Out;
    }

    if (const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Inst)) {
      if (Select->getType()->isIntegerTy()) {
        Out[Select] = evalSelect(Select, In);
      }
      return Out;
    }

    if (const auto *Phi = llvm::dyn_cast<llvm::PHINode>(Inst)) {
      if (Phi->getType()->isIntegerTy()) {
        Out[Phi] = evalPhi(Phi, In);
      }
      return Out;
    }

    if (const auto *Freeze = llvm::dyn_cast<llvm::FreezeInst>(Inst)) {
      if (Freeze->getType()->isIntegerTy()) {
        Out[Freeze] = resolveValue(In, Freeze->getOperand(0));
      }
      return Out;
    }

    if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(Inst)) {
      clobberMemoryByCall(Call, Out);
      if (Call->getType()->isIntegerTy()) {
        Out[Call] = SignValue::top();
      }
      return Out;
    }

    if (isIntegerLike(Inst)) {
      Out[Inst] = SignValue::top();
    }
    return Out;
  }

  SignMap initialFact() const override { return SignMap{}; }
};

} // namespace

SignAnalysisResult runIntraElimSignAnalysis(llvm::Function *F,
                                            EliminationOptions Opts) {
  if (F == nullptr || F->isDeclaration()) {
    return SignAnalysisResult{};
  }

  ElimSignAnalysisProblem Problem(F);
  IntraEliminationSolver<LLVMAnalysisTypes<SignMap, SignDomain>> Solver(Problem, Opts);
  auto Status = Solver.solve();
  auto Out = Solver.getResults();
  Out.setSolveMetadata(Status, Solver.getDiagnostics());
  return Out;
}

} // namespace elimination
