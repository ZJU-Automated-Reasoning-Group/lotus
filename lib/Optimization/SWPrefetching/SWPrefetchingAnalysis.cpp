#include "Optimization/SWPrefetching/SWPrefetchingInternal.h"

#include <algorithm>
#include <cassert>

namespace llvm {

bool SWPrefetchingLLVMPass::SearchAlgorithm(
    Instruction *I, LoopInfo &LI, Instruction *&Phi,
    SmallVector<Instruction *, 10> &Loads,
    SmallVector<Instruction *, 20> &Instrs,
    SmallVector<Instruction *, 10> &Phis) {
  bool PhiFound = false;
  Use *OperandList = I->getOperandList();
  Use *NumOfOperands = OperandList + I->getNumOperands();
  Loop *curInstrLoop = LI.getLoopFor(I->getParent());
  SmallVector<Instruction *, 10> NeedToSearch;

  for (Use *op = OperandList; op < NumOfOperands; op++) {
    if (PHINode *CurOpIsPhiNode = dyn_cast<PHINode>(op->get())) {
      Phi = CurOpIsPhiNode;
      if (!(std::find(Phis.begin(), Phis.end(), CurOpIsPhiNode) !=
            Phis.end())) {
        Phis.push_back(CurOpIsPhiNode);
      }
      PhiFound = true;
    } else if (LoadInst *curOperandIsLoad = dyn_cast<LoadInst>(op->get())) {
      if (!(std::find(Loads.begin(), Loads.end(), curOperandIsLoad) !=
            Loads.end())) {
        Loads.push_back(curOperandIsLoad);
      }
      NeedToSearch.push_back(curOperandIsLoad);
    } else if (Instruction *OtherTypeInstr = dyn_cast<Instruction>(op->get())) {
      Loop *OtherTypeInstrLoop = LI.getLoopFor(OtherTypeInstr->getParent());
      if (OtherTypeInstrLoop == curInstrLoop) {
        NeedToSearch.push_back(OtherTypeInstr);
      }
    }
  }

  for (size_t index = 0; index < NeedToSearch.size(); index++) {
    Instrs.push_back(NeedToSearch[index]);
    SearchAlgorithm(NeedToSearch[index], LI, Phi, Loads, Instrs, Phis);
    PhiFound = true;
  }
  return PhiFound;
}

bool SWPrefetchingLLVMPass::IsDep(
    Instruction *I, LoopInfo &LI, Instruction *&Phi,
    SmallVector<Instruction *, 10> &DependentLoadsToCurLoad,
    SmallVector<Instruction *, 20> &DependentInstrsToCurLoad,
    SmallVector<Instruction *, 10> &Phis) {
  bool PhiFound = false;
  Use *OperandList = I->getOperandList();
  Use *NumOfOperands = OperandList + I->getNumOperands();
  Loop *curInstrLoop = LI.getLoopFor(I->getParent());

  for (Use *op = OperandList; op < NumOfOperands; op++) {
    if (PHINode *CurOpIsPhiNode = dyn_cast<PHINode>(op->get())) {
      Loop *PhiNodeLoop = LI.getLoopFor(CurOpIsPhiNode->getParent());
      if (PhiNodeLoop == curInstrLoop) {
        Phi = CurOpIsPhiNode;
        DependentInstrsToCurLoad.push_back(CurOpIsPhiNode);
        Phis.push_back(CurOpIsPhiNode);
        PhiFound = true;
      }
    } else if (LoadInst *curOperandIsLoad = dyn_cast<LoadInst>(op->get())) {
      Loop *LoadInstrLoop = LI.getLoopFor(curOperandIsLoad->getParent());
      if (LoadInstrLoop == curInstrLoop) {
        DependentLoadsToCurLoad.push_back(curOperandIsLoad);
        DependentInstrsToCurLoad.push_back(curOperandIsLoad);
        if (IsDep(curOperandIsLoad, LI, Phi, DependentLoadsToCurLoad,
                  DependentInstrsToCurLoad, Phis)) {
          PhiFound = true;
        }
      }
    } else if (Instruction *OtherTypeInstr = dyn_cast<Instruction>(op->get())) {
      Loop *OtherTypeInstrLoop = LI.getLoopFor(OtherTypeInstr->getParent());
      if (OtherTypeInstrLoop == curInstrLoop) {
        DependentInstrsToCurLoad.push_back(OtherTypeInstr);
        if (IsDep(OtherTypeInstr, LI, Phi, DependentLoadsToCurLoad,
                  DependentInstrsToCurLoad, Phis)) {
          PhiFound = true;
        }
      }
    }
  }
  return PhiFound;
}

ConstantInt *
SWPrefetchingLLVMPass::getValueAddedToIndVarInLoopIterxxx(Loop *L) {
  SetVector<Instruction *> BBInsts;
  auto *B = L->getExitingBlock();
  if (!B) {
    return nullptr;
  }

  for (Instruction &J : *B) {
    BBInsts.insert(&J);
  }

  bool Changed = false;
  for (int i = BBInsts.size() - 1; i >= 0; i--) {
    CmpInst *CI = dyn_cast<CmpInst>(BBInsts[i]);
    if (CI) {
      Instruction *AddI = dyn_cast<Instruction>(BBInsts[i - 1]);
      if (AddI->getOpcode() == Instruction::Add) {
        if (L->makeLoopInvariant(AddI->getOperand(1), Changed)) {
          if (ConstantInt *constInt =
                  dyn_cast<ConstantInt>(AddI->getOperand(1))) {
            return constInt;
          }
        }
        if (L->makeLoopInvariant(AddI->getOperand(0), Changed)) {
          if (ConstantInt *constInt =
                  dyn_cast<ConstantInt>(AddI->getOperand(1))) {
            return constInt;
          }
        }
      }
    }
  }
  return nullptr;
}

PHINode *SWPrefetchingLLVMPass::getCanonicalishInductionVariable(Loop *L) {
  BasicBlock *H = L->getHeader();
  BasicBlock *Incoming = nullptr;
  BasicBlock *Backedge = nullptr;
  pred_iterator PI = pred_begin(H);
  assert(PI != pred_end(H) && "Loop must have at least one backedge!");
  Backedge = *PI++;
  if (PI == pred_end(H)) {
    return nullptr;
  }
  Incoming = *PI++;
  if (PI != pred_end(H)) {
    return nullptr;
  }
  if (L->contains(Incoming)) {
    if (L->contains(Backedge)) {
      return nullptr;
    }
    std::swap(Incoming, Backedge);
  } else if (!L->contains(Backedge)) {
    return nullptr;
  }
  for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
    PHINode *PN = cast<PHINode>(I);
    if (Instruction *Inc =
            dyn_cast<Instruction>(PN->getIncomingValueForBlock(Backedge))) {
      if (Inc->getOpcode() == Instruction::Add && Inc->getOperand(0) == PN) {
        if (dyn_cast<ConstantInt>(Inc->getOperand(1))) {
          return PN;
        }
      }
    }
  }
  return nullptr;
}

Value *SWPrefetchingLLVMPass::getLoopEndCondxxx(Loop *L) {
  SetVector<Instruction *> BBInsts;
  auto *B = L->getExitingBlock();
  if (!B) {
    return nullptr;
  }
  for (Instruction &J : *B) {
    BBInsts.insert(&J);
  }

  bool Changed = false;
  for (int i = BBInsts.size() - 1; i >= 0; i--) {
    CmpInst *CI = dyn_cast<CmpInst>(BBInsts[i]);
    if (CI) {
      if (L->makeLoopInvariant(CI->getOperand(1), Changed)) {
        return CI->getOperand(1);
      }
      if (L->makeLoopInvariant(CI->getOperand(0), Changed)) {
        return CI->getOperand(0);
      }
    }
  }
  return nullptr;
}

CmpInst *SWPrefetchingLLVMPass::getCompareInstrADD(Loop *L,
                                                   Instruction *nextInd) {
  SetVector<Instruction *> BBInsts;
  auto *B = L->getExitingBlock();
  if (!B) {
    return nullptr;
  }
  for (Instruction &J : *B) {
    BBInsts.insert(&J);
  }
  for (int i = BBInsts.size() - 1; i >= 0; i--) {
    CmpInst *CI = dyn_cast<CmpInst>(BBInsts[i]);
    if (CI && (CI->getOperand(0) == nextInd || CI->getOperand(1) == nextInd) &&
        nextInd->getOpcode() == Instruction::Add) {
      return CI;
    }
  }
  return nullptr;
}

CmpInst *SWPrefetchingLLVMPass::getCompareInstrGetElememntPtr(
    Loop *L, Instruction *nextInd) {
  SetVector<Instruction *> BBInsts;
  auto *B = L->getExitingBlock();
  if (!B) {
    return nullptr;
  }
  for (Instruction &J : *B) {
    BBInsts.insert(&J);
  }
  for (int i = BBInsts.size() - 1; i >= 0; i--) {
    CmpInst *CI = dyn_cast<CmpInst>(BBInsts[i]);
    if (CI && (CI->getOperand(0) == nextInd || CI->getOperand(1) == nextInd) &&
        nextInd->getOpcode() == Instruction::GetElementPtr) {
      return CI;
    }
  }
  return nullptr;
}

bool SWPrefetchingLLVMPass::CheckLoopCond(Loop *L) {
  bool OKtoPrefetch = false;
  BasicBlock *H = L->getHeader();
  BasicBlock *Incoming = nullptr;
  BasicBlock *Backedge = nullptr;
  pred_iterator PI = pred_begin(H);
  assert(PI != pred_end(H) && "Loop must have at least one backedge!");
  Backedge = *PI++;
  if (PI == pred_end(H)) {
    return OKtoPrefetch;
  }
  Incoming = *PI++;
  if (PI != pred_end(H)) {
    return OKtoPrefetch;
  }

  if (L->contains(Incoming)) {
    if (L->contains(Backedge)) {
      return OKtoPrefetch;
    }
    std::swap(Incoming, Backedge);
  } else if (!L->contains(Backedge)) {
    return OKtoPrefetch;
  }
  OKtoPrefetch = true;
  return OKtoPrefetch;
}

Instruction *SWPrefetchingLLVMPass::GetIncomingValue(Loop *L,
                                                     Instruction *curPN) {
  BasicBlock *H = L->getHeader();
  BasicBlock *Backedge = nullptr;
  pred_iterator PI = pred_begin(H);
  Backedge = *PI++;

  for (BasicBlock::iterator I = H->begin(); isa<PHINode>(I); ++I) {
    PHINode *PN = cast<PHINode>(I);
    if (PN == curPN) {
      if (Instruction *IncomingInstr =
              dyn_cast<Instruction>(PN->getIncomingValueForBlock(Backedge))) {
        return IncomingInstr;
      }
    }
  }
  return nullptr;
}

ConstantInt *SWPrefetchingLLVMPass::getValueAddedToIndVar(Loop *L,
                                                          Instruction *nextInd) {
  bool Changed = false;
  if (L->makeLoopInvariant(nextInd->getOperand(1), Changed)) {
    if (ConstantInt *constInt = dyn_cast<ConstantInt>(nextInd->getOperand(1))) {
      return constInt;
    }
  }
  if (L->makeLoopInvariant(nextInd->getOperand(0), Changed)) {
    if (ConstantInt *constInt = dyn_cast<ConstantInt>(nextInd->getOperand(1))) {
      return constInt;
    }
  }
  return nullptr;
}

} // namespace llvm
