#include "Optimization/SWPrefetching/SWPrefetchingInternal.h"

#include <algorithm>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"

namespace llvm {

bool SWPrefetchingLLVMPass::InjectPrefechesOnePhiPartTwo(
    Instruction *I, LoopInfo &LI, Instruction *Phi,
    SmallVector<Instruction *, 20> &DepInstrs, int64_t prefetchDist) {
  bool done = false;
  bool nonCanonical = false;
  Instruction *phi = Phi;
  ValueMap<Instruction *, Value *> Transforms;
  IRBuilder<> Builder(I);

  Loop *curLoop = LI.getLoopFor(phi->getParent());
  if (!getLoopEndCondxxx(curLoop)) {
    SmallVector<Instruction *, 4> DepPhiInsts;
    for (auto &curDep : DepInstrs) {
      if (curDep == phi) {
        SetVector<Instruction *> BBInsts;
        auto *B = curLoop->getExitingBlock();
        if (B) {
          for (Instruction &J : *B) {
            BBInsts.insert(&J);
          }
          for (int i = BBInsts.size() - 1; i >= 0; i--) {
            CmpInst *CI = dyn_cast<CmpInst>(BBInsts[i]);
            if (CI) {
              DepPhiInsts.push_back(CI);
              Use *OperandList = CI->getOperandList();
              Use *NumOfOperands = OperandList + CI->getNumOperands();
              for (Use *op = OperandList; op < NumOfOperands; op++) {
                if (dyn_cast<Instruction>(op->get())) {
                  Instruction *OPInstr = dyn_cast<Instruction>(op->get());
                  DepPhiInsts.push_back(OPInstr);
                }
              }
            }
          }
        }
        if (curDep == getCanonicalishInductionVariable(curLoop)) {
          Instruction *NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(
              curDep,
              curDep->getType()->isIntegerTy(64)
                  ? ConstantInt::get(
                        Type::getInt64Ty(
                            (curDep->getFunction())->getParent()->getContext()),
                        prefetchDist)
                  : ConstantInt::get(
                        Type::getInt32Ty(((curDep->getFunction())->getParent())
                                             ->getContext()),
                        prefetchDist)));
          Transforms.insert(
              std::pair<Instruction *, Instruction *>(curDep, NewInstr));
          for (auto &s : DepPhiInsts) {
            DepInstrs.push_back(s);
          }
          for (auto &s : DepPhiInsts) {
            Use *OpsInstr = s->getOperandList();
            int64_t sNumOp = s->getNumOperands();
            for (int64_t index = 0; index < sNumOp; index++) {
              Value *ops = OpsInstr[index].get();
              Instruction *m = dyn_cast<Instruction>(ops);
              if (!(std::find(DepInstrs.begin(), DepInstrs.end(), m) !=
                    DepInstrs.end())) {
                if (!(dyn_cast<ConstantInt>(ops))) {
                  DepInstrs.push_back(m);
                }
              }
            }
          }
        }
      }
    }
  } else {
    for (auto &curDep : DepInstrs) {
      if (Transforms.count(curDep)) {
        continue;
      }
      if (curDep == phi) {
        if (curDep == getCanonicalishInductionVariable(curLoop)) {
          Value *EndCond = getLoopEndCondxxx(curLoop);
          Instruction *IncInstr = GetIncomingValue(curLoop, phi);
          ConstantInt *UpdateInd = getValueAddedToIndVar(curLoop, IncInstr);
          Instruction *mod;
          if (UpdateInd->isNegative()) {
            int64_t curprefetchDist = 0 - prefetchDist;
            Instruction *NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(
                curDep, curDep->getType()->isIntegerTy(64)
                            ? ConstantInt::get(
                                  Type::getInt64Ty(
                                      ((curDep->getFunction())->getParent())
                                          ->getContext()),
                                  curprefetchDist)
                            : ConstantInt::get(
                                  Type::getInt32Ty(
                                      ((curDep->getFunction())->getParent())
                                          ->getContext()),
                                  curprefetchDist)));
            if (EndCond->getType() != NewInstr->getType()) {
              Instruction *cast = CastInst::CreateIntegerCast(
                  EndCond, NewInstr->getType(), true);
              Builder.Insert(cast);
              Value *cmp =
                  Builder.CreateICmp(CmpInst::ICMP_SGT, cast, NewInstr);
              mod = dyn_cast<Instruction>(
                  Builder.CreateSelect(cmp, cast, NewInstr));
            } else {
              Value *cmp =
                  Builder.CreateICmp(CmpInst::ICMP_SGT, EndCond, NewInstr);
              mod = dyn_cast<Instruction>(
                  Builder.CreateSelect(cmp, EndCond, NewInstr));
            }
            Transforms.insert(
                std::pair<Instruction *, Instruction *>(curDep, NewInstr));
          } else {
            Instruction *NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(
                curDep, curDep->getType()->isIntegerTy(64)
                            ? ConstantInt::get(
                                  Type::getInt64Ty(
                                      ((curDep->getFunction())->getParent())
                                          ->getContext()),
                                  prefetchDist)
                            : ConstantInt::get(
                                  Type::getInt32Ty(
                                      ((curDep->getFunction())->getParent())
                                          ->getContext()),
                                  prefetchDist)));
            if (EndCond->getType() != NewInstr->getType()) {
              Instruction *cast = CastInst::CreateIntegerCast(
                  EndCond, NewInstr->getType(), true);
              Builder.Insert(cast);
              Value *cmp =
                  Builder.CreateICmp(CmpInst::ICMP_SLT, cast, NewInstr);
              mod = dyn_cast<Instruction>(
                  Builder.CreateSelect(cmp, cast, NewInstr));
            } else {
              Value *cmp =
                  Builder.CreateICmp(CmpInst::ICMP_SLT, EndCond, NewInstr);
              mod = dyn_cast<Instruction>(
                  Builder.CreateSelect(cmp, EndCond, NewInstr));
            }
            Transforms.insert(
                std::pair<Instruction *, Instruction *>(curDep, mod));
          }
        } else {
          nonCanonical = true;
        }
      }
    }
  }

  if (nonCanonical) {
    GetElementPtrInst *newPhi = dyn_cast<GetElementPtrInst>(
        Builder.CreateInBoundsGEP(
            phi->getType(), phi,
            phi->getType()->isIntegerTy(64)
                ? ConstantInt::get(
                      Type::getInt64Ty(
                          ((phi->getFunction())->getParent())->getContext()),
                      prefetchDist)
                : ConstantInt::get(
                      Type::getInt32Ty(
                          ((phi->getFunction())->getParent())->getContext()),
                      prefetchDist)));
    Transforms.insert(
        std::pair<Instruction *, GetElementPtrInst *>(phi, newPhi));
  }

  for (int index = DepInstrs.size() - 1; index >= 0; index--) {
    auto &curDep = DepInstrs[index];
    if (!dyn_cast<PHINode>(curDep)) {
      Instruction *NewInstr = curDep->clone();
      Use *OpListNewInstr = NewInstr->getOperandList();
      int64_t NewInstrsNumOp = NewInstr->getNumOperands();
      for (int64_t operandIndex = 0; operandIndex < NewInstrsNumOp;
           operandIndex++) {
        Value *op = OpListNewInstr[operandIndex].get();
        if (dyn_cast<GetElementPtrInst>(op)) {
          GetElementPtrInst *opIsInstr = dyn_cast<GetElementPtrInst>(op);
          if (Transforms.count(opIsInstr)) {
            NewInstr->setOperand(operandIndex, Transforms.lookup(opIsInstr));
          }
        } else if (Instruction *opIsInstr = dyn_cast<Instruction>(op)) {
          if (Transforms.count(opIsInstr)) {
            NewInstr->setOperand(operandIndex, Transforms.lookup(opIsInstr));
          }
        }
      }
      NewInstr->insertBefore(I);
      Transforms.insert(
          std::pair<Instruction *, Instruction *>(curDep, NewInstr));
    }
  }

  Type *I8 =
      Type::getInt8PtrTy(((I->getFunction())->getParent())->getContext());
  Type *I32 = Type::getInt32Ty((I->getParent())->getContext());
  Function *PrefetchFunc = Intrinsic::getDeclaration(
      (I->getFunction())->getParent(), Intrinsic::prefetch, I8);
  Instruction *oldGep = dyn_cast<Instruction>(I->getOperand(0));
  Instruction *gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
  Instruction *cast = dyn_cast<Instruction>(Builder.CreateBitCast(
      gep,
      Type::getInt8PtrTy(((I->getFunction())->getParent())->getContext())));
  Value *ar[] = {cast, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3),
                 ConstantInt::get(I32, 1)};
  CallInst *call = CallInst::Create(PrefetchFunc, ar);
  call->insertBefore(I);
  done = true;
  return done;
}

bool SWPrefetchingLLVMPass::InjectPrefechesOnePhiPartOne(
    Instruction *I, LoopInfo &LI, SmallVector<Instruction *, 10> &Phi,
    SmallVector<Instruction *, 10> &CapturedLoads,
    SmallVector<Instruction *, 20> &DepInstrs, int64_t prefetchDist,
    bool ItIsIndirectLoad) {
  bool done = false;
  Instruction *phi = nullptr;
  SmallVector<Instruction *, 10> DependentLoadsToCurLoadx;
  SmallVector<Instruction *, 20> DependentInstrsToCurLoadx;
  SmallVector<Instruction *, 10> DependentPhisx;

  (void)Phi;
  (void)CapturedLoads;
  (void)ItIsIndirectLoad;

  if (IsDep(I, LI, phi, DependentLoadsToCurLoadx, DependentInstrsToCurLoadx,
            DependentPhisx)) {
    Instruction *SearchPhi = nullptr;
    SmallVector<Instruction *, 10> SearchLoads;
    SmallVector<Instruction *, 20> SearchInstrs;
    SmallVector<Instruction *, 10> SearchPhis;
    for (int index = DependentLoadsToCurLoadx.size() - 1; index >= 0; index--) {
      if (IsDep(DependentLoadsToCurLoadx[index], LI, SearchPhi, SearchLoads,
                SearchInstrs, SearchPhis)) {
        if (DependentPhisx[0] == SearchPhis[0]) {
          if (InjectPrefechesOnePhiPartTwo(I, LI, DependentPhisx[0],
                                           DependentInstrsToCurLoadx,
                                           prefetchDist)) {
            done = true;
          }
          if (InjectPrefechesOnePhiPartTwo(DependentLoadsToCurLoadx[index], LI,
                                           SearchPhis[0], SearchInstrs,
                                           prefetchDist * 2)) {
            done = true;
          }
        }
      }
    }
  }
  return done;
}

} // namespace llvm
