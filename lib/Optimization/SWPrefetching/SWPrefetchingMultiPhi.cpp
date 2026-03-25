#include "Optimization/SWPrefetching/SWPrefetchingInternal.h"

#include <algorithm>
#include <vector>

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

namespace llvm {

bool SWPrefetchingLLVMPass::InjectPrefeches(
    Instruction *curLoad, LoopInfo &LI,
    SmallVector<Instruction *, 10> &CapturedPhis,
    SmallVector<Instruction *, 10> &CapturedLoads,
    SmallVector<Instruction *, 20> &CapturedInstrs, int64_t prefetchDist,
    bool ItIsIndirectLoad) {
  Loop *IndirectLoadLoop;
  if (ItIsIndirectLoad) {
    IndirectLoad = curLoad;
    IndirectLoads = CapturedLoads;
    IndirectInstrs = CapturedInstrs;
    IndirectPhis = CapturedPhis;
    IndirectPrefetchDist = prefetchDist;
    IndirectLoadLoop = LI.getLoopFor(IndirectLoad->getParent());
  }

  bool done = false;
  bool PrefetchGetElem = false;
  Instruction *phi = nullptr;
  Loop *curLoadLoop = LI.getLoopFor(curLoad->getParent());
  bool donePrefetchingForPhi = false;

  if (CapturedPhis.size() == 1) {
    phi = CapturedPhis[0];
    ValueMap<Instruction *, Value *> Transforms;
    IRBuilder<> Builder(curLoad);
    Loop *PhiLoop = LI.getLoopFor(phi->getParent());

    for (auto &curDep : CapturedInstrs) {
      if (Transforms.count(curDep)) {
        continue;
      }
      if (curDep == phi) {
        if (PhiLoop == curLoadLoop) {
          if (CheckLoopCond(PhiLoop)) {
            if (GetIncomingValue(PhiLoop, phi)) {
              Instruction *IncInstr = GetIncomingValue(PhiLoop, phi);
              if (IncInstr->getOpcode() == Instruction::Add &&
                  IncInstr->getOperand(0) == phi) {
                errs() << "ADD\n";
                Instruction *NewInstr;
                Instruction *mod;
                if (dyn_cast<ConstantInt>(IncInstr->getOperand(1))) {
                  if (getCompareInstrADD(PhiLoop, IncInstr)) {
                    Value *EndCond = nullptr;
                    bool Changed = false;
                    CmpInst *compareInstr =
                        getCompareInstrADD(PhiLoop, IncInstr);
                    if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(0),
                                                   Changed)) {
                      EndCond = compareInstr->getOperand(0);
                    }
                    if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(1),
                                                   Changed)) {
                      EndCond = compareInstr->getOperand(1);
                    }
                    ConstantInt *UpdateInd =
                        getValueAddedToIndVar(PhiLoop, IncInstr);
                    if (UpdateInd) {
                      if (UpdateInd->isNegative()) {
                        int64_t curPrefetchDist = 0 - prefetchDist;
                        NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(
                            curDep,
                            curDep->getType()->isIntegerTy(64)
                                ? ConstantInt::get(
                                      Type::getInt64Ty(
                                          (curDep->getParent())->getContext()),
                                      curPrefetchDist)
                                : ConstantInt::get(
                                      Type::getInt32Ty(
                                          (curDep->getParent())->getContext()),
                                      curPrefetchDist)));

                        if (EndCond != nullptr) {
                          if (EndCond->getType() != NewInstr->getType()) {
                            Instruction *cast = CastInst::CreateIntegerCast(
                                EndCond, NewInstr->getType(), true);
                            Builder.Insert(cast);
                            Value *cmp = Builder.CreateICmp(CmpInst::ICMP_SGT,
                                                            cast, NewInstr);
                            mod = dyn_cast<Instruction>(
                                Builder.CreateSelect(cmp, cast, NewInstr));
                          } else {
                            Value *cmp = Builder.CreateICmp(CmpInst::ICMP_SGT,
                                                            EndCond, NewInstr);
                            mod = dyn_cast<Instruction>(
                                Builder.CreateSelect(cmp, EndCond, NewInstr));
                          }
                        }
                        Transforms.insert(
                            std::pair<Instruction *, Instruction *>(curDep,
                                                                    NewInstr));
                        donePrefetchingForPhi = true;
                      } else {
                        NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(
                            curDep,
                            curDep->getType()->isIntegerTy(64)
                                ? ConstantInt::get(
                                      Type::getInt64Ty(
                                          (curDep->getParent())->getContext()),
                                      prefetchDist)
                                : ConstantInt::get(
                                      Type::getInt32Ty(
                                          (curDep->getParent())->getContext()),
                                      prefetchDist)));
                        if (EndCond != nullptr) {
                          if (EndCond->getType() != NewInstr->getType()) {
                            Instruction *cast = CastInst::CreateIntegerCast(
                                EndCond, NewInstr->getType(), true);
                            Builder.Insert(cast);
                            Value *cmp = Builder.CreateICmp(CmpInst::ICMP_SLT,
                                                            cast, NewInstr);
                            mod = dyn_cast<Instruction>(
                                Builder.CreateSelect(cmp, cast, NewInstr));
                          } else {
                            Value *cmp = Builder.CreateICmp(CmpInst::ICMP_SLT,
                                                            EndCond, NewInstr);
                            mod = dyn_cast<Instruction>(
                                Builder.CreateSelect(cmp, EndCond, NewInstr));
                          }
                          Transforms.insert(
                              std::pair<Instruction *, Instruction *>(curDep,
                                                                      mod));
                        } else {
                          Transforms.insert(
                              std::pair<Instruction *, Instruction *>(
                                  curDep, NewInstr));
                        }
                        donePrefetchingForPhi = true;
                      }
                    }
                  }
                }
              }
              if (IncInstr->getOpcode() == Instruction::Mul &&
                  IncInstr->getOperand(0) == phi) {
                errs() << "Mul\n";
              }

              if (IncInstr->getOpcode() == Instruction::GetElementPtr &&
                  IncInstr->getOperand(0) == phi) {
                GetElementPtrInst *NewInstr;
                SmallVector<Instruction *, 10> NextPhiDependencies;
                NextPhiDependencies.push_back(IncInstr);
                if (dyn_cast<ConstantInt>(IncInstr->getOperand(1))) {
                  if (getCompareInstrGetElememntPtr(PhiLoop, IncInstr)) {
                    Value *EndCond;
                    bool Changed = false;
                    CmpInst *compareInstr =
                        getCompareInstrGetElememntPtr(PhiLoop, IncInstr);
                    if (PhiLoop->makeLoopInvariant(compareInstr->getOperand(0),
                                                   Changed)) {
                      EndCond = compareInstr->getOperand(0);
                      NextPhiDependencies.push_back(
                          dyn_cast<Instruction>(compareInstr->getOperand(0)));
                    } else if (PhiLoop->makeLoopInvariant(
                                   compareInstr->getOperand(1), Changed)) {
                      EndCond = compareInstr->getOperand(1);
                      NextPhiDependencies.push_back(
                          dyn_cast<Instruction>(compareInstr->getOperand(1)));
                    } else if (compareInstr->getOperand(1) != IncInstr &&
                               compareInstr->getOperand(0) == IncInstr) {
                      EndCond = compareInstr->getOperand(1);
                      NextPhiDependencies.push_back(
                          dyn_cast<Instruction>(compareInstr->getOperand(1)));
                    } else if (compareInstr->getOperand(0) != IncInstr &&
                               compareInstr->getOperand(0) == IncInstr) {
                      EndCond = compareInstr->getOperand(0);
                      NextPhiDependencies.push_back(
                          dyn_cast<Instruction>(compareInstr->getOperand(0)));
                    }

                    (void)EndCond;
                    NextPhiDependencies.push_back(compareInstr);
                    getValueAddedToIndVar(PhiLoop, IncInstr);
                    for (size_t index = 0; index < NextPhiDependencies.size();
                         index++) {
                      if (NextPhiDependencies[index]->getOpcode() ==
                          Instruction::GetElementPtr) {
                        if ((NextPhiDependencies[index]->getOperand(0) ==
                                 curDep ||
                             NextPhiDependencies[index]->getOperand(1) ==
                                 curDep)) {
                          NewInstr = dyn_cast<GetElementPtrInst>(
                              Builder.CreateInBoundsGEP(
                                  curDep->getType(), curDep,
                                  curDep->getType()->isIntegerTy(64)
                                      ? ConstantInt::get(
                                            Type::getInt64Ty(
                                                (curDep->getParent())
                                                    ->getContext()),
                                            prefetchDist)
                                      : ConstantInt::get(
                                            Type::getInt32Ty(
                                                (curDep->getParent())
                                                    ->getContext()),
                                            prefetchDist)));
                          Transforms.insert(
                              std::pair<Instruction *, GetElementPtrInst *>(
                                  curDep, NewInstr));
                          donePrefetchingForPhi = true;
                          PrefetchGetElem = true;
                        }
                      }
                    }
                  } else {
                    NewInstr =
                        dyn_cast<GetElementPtrInst>(Builder.CreateInBoundsGEP(
                            phi->getType(), phi,
                            ConstantInt::get(
                                Type::getInt64Ty(
                                    (curDep->getParent())->getContext()),
                                prefetchDist)));
                    Transforms.insert(
                        std::pair<Instruction *, GetElementPtrInst *>(
                            phi, NewInstr));
                    donePrefetchingForPhi = true;
                    PrefetchGetElem = true;
                  }
                }
              }
            }
          } else {
            return done;
          }
        }
      }
    }
    if (donePrefetchingForPhi) {
      for (int index = CapturedInstrs.size() - 1; index >= 0; index--) {
        auto &curDep = CapturedInstrs[index];
        if (!dyn_cast<PHINode>(curDep)) {
          Instruction *NewInstr = curDep->clone();
          Use *OpListNewInstr = NewInstr->getOperandList();
          int64_t NewInstrsNumOp = NewInstr->getNumOperands();
          for (int64_t index = 0; index < NewInstrsNumOp; index++) {
            Value *op = OpListNewInstr[index].get();
            if (dyn_cast<GetElementPtrInst>(op)) {
              GetElementPtrInst *opIsInstr = dyn_cast<GetElementPtrInst>(op);
              if (Transforms.count(opIsInstr)) {
                NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
              }
            } else if (Instruction *opIsInstr = dyn_cast<Instruction>(op)) {
              if (Transforms.count(opIsInstr)) {
                NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
              }
            }
          }
          NewInstr->insertBefore(curLoad);
          Transforms.insert(
              std::pair<Instruction *, Instruction *>(curDep, NewInstr));
        }
      }
      if (!PrefetchGetElem) {
        Type *I32 = Type::getInt32Ty((curLoad->getParent())->getContext());
        Type *I8 = Type::getInt8PtrTy(
            ((curLoad->getFunction())->getParent())->getContext());
        Function *PrefetchFunc = Intrinsic::getDeclaration(
            (curLoad->getFunction())->getParent(), Intrinsic::prefetch, I8);
        Instruction *oldGep = dyn_cast<Instruction>(curLoad->getOperand(0));
        Instruction *gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
        Instruction *cast = dyn_cast<Instruction>(Builder.CreateBitCast(
            gep, Type::getInt8PtrTy(
                     ((curLoad->getFunction())->getParent())->getContext())));
        Value *ar[] = {cast, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3),
                       ConstantInt::get(I32, 1)};
        CallInst *call = CallInst::Create(PrefetchFunc, ar);
        call->insertBefore(curLoad);
      } else {
        Type *I32 = Type::getInt32Ty((curLoad->getParent())->getContext());
        Function *PrefetchFunc = Intrinsic::getDeclaration(
            (curLoad->getFunction())->getParent(), Intrinsic::prefetch,
            (curLoad->getOperand(0))->getType());
        Instruction *oldGep = dyn_cast<Instruction>(curLoad->getOperand(0));
        Instruction *gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
        Value *ar[] = {gep, ConstantInt::get(I32, 0), ConstantInt::get(I32, 3),
                       ConstantInt::get(I32, 1)};
        CallInst *call = CallInst::Create(PrefetchFunc, ar);
        call->insertBefore(curLoad);
      }
      if (IndirectLoads.size() > 0) {
        for (size_t index = 0; index < IndirectLoads.size(); index++) {
          auto &curStrideLoad = IndirectLoads[index];
          Loop *curStrideLoadLoop = LI.getLoopFor(curStrideLoad->getParent());
          if (curStrideLoadLoop == IndirectLoadLoop) {
            bool ItIsStrideLoad = false;
            Instruction *StridePhi = nullptr;
            SmallVector<Instruction *, 10> StrideLoads;
            SmallVector<Instruction *, 20> StrideInstrs;
            SmallVector<Instruction *, 10> StridePhis;
            int64_t StridePrefetchDist;
            if (SearchAlgorithm(curStrideLoad, LI, StridePhi, StrideLoads,
                                StrideInstrs, StridePhis)) {
              for (size_t index = 0; index < StridePhis.size(); index++) {
                StrideInstrs.push_back(
                    StridePhis[StridePhis.size() - 1 - index]);
              }
              bool NotFoundAPhi = false;
              for (size_t j = 0; j < StridePhis.size(); j++) {
                if (!(std::find(IndirectPhis.begin(), IndirectPhis.end(),
                                StridePhis[j]) != IndirectPhis.end())) {
                  NotFoundAPhi = true;
                }
              }
              bool NotFoundAnInstr = false;
              for (size_t j = 0; j < StrideInstrs.size(); j++) {
                if (!(std::find(IndirectInstrs.begin(), IndirectInstrs.end(),
                                StrideInstrs[j]) != IndirectInstrs.end())) {
                  NotFoundAnInstr = true;
                }
              }
              if (!NotFoundAnInstr && !NotFoundAPhi) {
                ItIsStrideLoad = true;
                StridePrefetchDist = IndirectPrefetchDist * (index + 2);
              }
              if (ItIsStrideLoad) {
                if (InjectPrefeches(curStrideLoad, LI, StridePhis, StrideLoads,
                                    StrideInstrs, StridePrefetchDist, false)) {
                  done = true;
                }
              }
            }
          }
        }
      }

      done = true;
    }
  } else {
    if (prefetchDist > 1000) {
      prefetchDist = prefetchDist / 1000;
      Loop *curPLoop;
      Loop *curLoop;
      std::vector<Instruction *> trans_new_instructions;
      std::vector<Instruction *> old_trans_new_instructions;
      std::vector<Instruction *> new_instructions;
      ValueToValueMapTy vmap;
      ValueMap<Instruction *, Value *> Transforms;
      Instruction *last;
      Instruction *cmp;
      Instruction *x;

      for (auto *p : CapturedPhis) {
        curPLoop = LI.getLoopFor(p->getParent());
        curLoop = LI.getLoopFor(curLoad->getParent());
        if (curPLoop != curLoop) {
          phi = p;
          curPLoop = LI.getLoopFor(p->getParent());
          auto *PEB = curPLoop->getExitingBlock();
          if (PEB) {
            SmallVector<Instruction *, 8> DepPhiInsts;
            SetVector<Instruction *> PEBInsts;
            if (PEB) {
              for (Instruction &J : *PEB) {
                PEBInsts.insert(&J);
              }
            }
            bool CIexist = false;
            for (int i = PEBInsts.size() - 1; i >= 0; i--) {
              cmp = dyn_cast<CmpInst>(PEBInsts[i]);
              if (cmp) {
                CIexist = true;
              }
            }
            if (CIexist) {
              for (int i = PEBInsts.size() - 1; i >= 0; i--) {
                if (!dyn_cast<BranchInst>(PEBInsts[i]) &&
                    !dyn_cast<CallInst>(PEBInsts[i]) &&
                    !dyn_cast<PHINode>(PEBInsts[i])) {
                  DepPhiInsts.push_back(PEBInsts[i]);
                }
              }
            }

            if (DepPhiInsts.size() > 0) {
              Instruction *insertPt = phi->getParent()->getFirstNonPHIOrDbg();
              for (int i = DepPhiInsts.size() - 1; i >= 0; i--) {
                auto *inst = DepPhiInsts[i];
                auto *new_inst = inst->clone();
                if (new_inst->getOpcode() == Instruction::Add) {
                  Value *val;
                  if (new_inst->getOperand(0)->getType()->isIntegerTy(64)) {
                    val = ConstantInt::get(
                        Type::getInt64Ty((curLoad->getParent())->getContext()),
                        prefetchDist);
                  } else {
                    val = ConstantInt::get(
                        Type::getInt32Ty((curLoad->getParent())->getContext()),
                        prefetchDist);
                  }
                  new_inst->setOperand(1, val);
                }
                new_inst->insertBefore(insertPt);
                new_instructions.push_back(new_inst);
                vmap[inst] = new_inst;
                last = new_inst;
                insertPt = new_inst->getNextNode();
              }

              for (auto *i : new_instructions) {
                RemapInstruction(i, vmap,
                                 RF_NoModuleLevelChanges |
                                     RF_IgnoreMissingLocals);
                if (dyn_cast<CmpInst>(i)) {
                  cmp = dyn_cast<CmpInst>(i);
                }
              }
            }

            IRBuilder<> Builder(last->getNextNode());
            Instruction *NewInstr = dyn_cast<Instruction>(Builder.CreateAdd(
                phi, phi->getType()->isIntegerTy(64)
                         ? ConstantInt::get(
                               Type::getInt64Ty(
                                   (curLoad->getParent())->getContext()),
                               prefetchDist)
                         : ConstantInt::get(
                               Type::getInt32Ty(
                                   (curLoad->getParent())->getContext()),
                               prefetchDist)));
            Transforms.insert(
                std::pair<Instruction *, Instruction *>(phi, NewInstr));
            x = NewInstr;
            SmallVector<Instruction *, 20> SDepInstrs_insideLoop;
            for (int index = CapturedInstrs.size() - 1; index >= 0; index--) {
              if (LI.getLoopFor(curLoad->getParent()) ==
                  LI.getLoopFor(CapturedInstrs[index]->getParent())) {
                SDepInstrs_insideLoop.push_back(CapturedInstrs[index]);
              }
            }
            bool theSLoad = false;
            Instruction *SLoad = nullptr;
            for (auto &t : SDepInstrs_insideLoop) {
              if (dyn_cast<LoadInst>(t)) {
                theSLoad = true;
                SLoad = t;
              }
            }
            Instruction *Sphi = nullptr;
            SmallVector<Instruction *, 10> SLoads;
            SmallVector<Instruction *, 20> SInstrs;
            SmallVector<Instruction *, 10> SPhis;

            if (theSLoad) {
              if (SearchAlgorithm(SLoad, LI, Sphi, SLoads, SInstrs, SPhis)) {
                for (size_t index = 0; index < SPhis.size(); index++) {
                  SInstrs.push_back(SPhis[SPhis.size() - 1 - index]);
                }
              }
            }

            SmallVector<Instruction *, 20> InstrsToInsert;
            bool phiFound = false;
            int start_index = 0;
            if (theSLoad) {
              InstrsToInsert = SInstrs;
            } else {
              InstrsToInsert = CapturedInstrs;
            }
            for (int index = InstrsToInsert.size() - 1; index >= 0; index--) {
              if (!phiFound) {
                auto &curDep = InstrsToInsert[index];
                Use *OpListNewInstr = curDep->getOperandList();
                int64_t NewInstrsNumOp = curDep->getNumOperands();
                for (int64_t i = 0; i < NewInstrsNumOp; i++) {
                  if (OpListNewInstr[i].get() == phi) {
                    phiFound = true;
                    start_index = index;
                  }
                }
              }
            }

            Instruction *last_gap = nullptr;
            auto &curDep = InstrsToInsert[start_index];
            if (!dyn_cast<PHINode>(curDep)) {
              Instruction *NewInstr = curDep->clone();
              Use *OpListNewInstr = NewInstr->getOperandList();
              int64_t NewInstrsNumOp = NewInstr->getNumOperands();
              for (int64_t index = 0; index < NewInstrsNumOp; index++) {
                Value *op = OpListNewInstr[index].get();
                if (dyn_cast<GetElementPtrInst>(op)) {
                  GetElementPtrInst *opIsInstr =
                      dyn_cast<GetElementPtrInst>(op);
                  if (Transforms.count(opIsInstr)) {
                    NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
                  }
                } else if (Instruction *opIsInstr = dyn_cast<Instruction>(op)) {
                  if (Transforms.count(opIsInstr)) {
                    NewInstr->setOperand(index, Transforms.lookup(opIsInstr));
                  }
                }
              }
              NewInstr->insertAfter(x);
              last_gap = NewInstr;
              Transforms.insert(
                  std::pair<Instruction *, Instruction *>(curDep, NewInstr));
              trans_new_instructions.push_back(NewInstr);
              old_trans_new_instructions.push_back(curDep);
              x = NewInstr;
            }
            for (int index = start_index - 1; index >= 0; index--) {
              bool insert = false;
              auto &curDep2 = InstrsToInsert[index];
              if (!dyn_cast<PHINode>(curDep2)) {
                if (dyn_cast<GetElementPtrInst>(curDep2)) {
                  Instruction *temp = dyn_cast<GetElementPtrInst>(curDep2);
                  last_gap = temp;
                  if ((std::find(CapturedPhis.begin(), CapturedPhis.end(),
                                 temp->getOperand(1)) != CapturedPhis.end())) {
                    Value *val = ConstantInt::get(
                        Type::getInt64Ty(((curLoad->getFunction())->getParent())
                                             ->getContext()),
                        0);
                    Instruction *NewInstr = curDep2->clone();
                    NewInstr->setOperand(1, val);
                    Use *OpListNewInstr = NewInstr->getOperandList();
                    int64_t NewInstrsNumOp = NewInstr->getNumOperands();
                    for (int64_t idx = 0; idx < NewInstrsNumOp; idx++) {
                      Value *op = OpListNewInstr[idx].get();
                      if (dyn_cast<GetElementPtrInst>(op)) {
                        GetElementPtrInst *opIsInstr =
                            dyn_cast<GetElementPtrInst>(op);
                        if (Transforms.count(opIsInstr)) {
                          NewInstr->setOperand(idx,
                                               Transforms.lookup(opIsInstr));
                          insert = true;
                        }
                      } else if (Instruction *opIsInstr =
                                     dyn_cast<Instruction>(op)) {
                        if (Transforms.count(opIsInstr)) {
                          NewInstr->setOperand(idx,
                                               Transforms.lookup(opIsInstr));
                          insert = true;
                        }
                      }
                    }
                    (void)insert;
                    NewInstr->insertAfter(x);
                    Transforms.insert(std::pair<Instruction *, Instruction *>(
                        curDep2, NewInstr));
                    trans_new_instructions.push_back(NewInstr);
                    old_trans_new_instructions.push_back(curDep2);
                    x = NewInstr;
                  } else {
                    Instruction *NewInstr = curDep2->clone();
                    Use *OpListNewInstr = NewInstr->getOperandList();
                    int64_t NewInstrsNumOp = NewInstr->getNumOperands();
                    for (int64_t idx = 0; idx < NewInstrsNumOp; idx++) {
                      Value *op = OpListNewInstr[idx].get();
                      if (dyn_cast<GetElementPtrInst>(op)) {
                        GetElementPtrInst *opIsInstr =
                            dyn_cast<GetElementPtrInst>(op);
                        if (Transforms.count(opIsInstr)) {
                          NewInstr->setOperand(idx,
                                               Transforms.lookup(opIsInstr));
                          insert = true;
                        }
                      } else if (Instruction *opIsInstr =
                                     dyn_cast<Instruction>(op)) {
                        if (Transforms.count(opIsInstr)) {
                          NewInstr->setOperand(idx,
                                               Transforms.lookup(opIsInstr));
                          insert = true;
                        }
                      }
                    }
                    (void)insert;
                    NewInstr->insertAfter(x);
                    Transforms.insert(std::pair<Instruction *, Instruction *>(
                        curDep2, NewInstr));
                    trans_new_instructions.push_back(NewInstr);
                    old_trans_new_instructions.push_back(curDep2);
                    x = NewInstr;
                  }
                } else {
                  bool insert = false;
                  Instruction *NewInstr = curDep2->clone();
                  Use *OpListNewInstr = NewInstr->getOperandList();
                  int64_t NewInstrsNumOp = NewInstr->getNumOperands();
                  for (int64_t idx = 0; idx < NewInstrsNumOp; idx++) {
                    Value *op = OpListNewInstr[idx].get();
                    if (dyn_cast<GetElementPtrInst>(op)) {
                      GetElementPtrInst *opIsInstr =
                          dyn_cast<GetElementPtrInst>(op);
                      if (Transforms.count(opIsInstr)) {
                        NewInstr->setOperand(idx, Transforms.lookup(opIsInstr));
                        insert = true;
                      }
                    } else if (Instruction *opIsInstr =
                                   dyn_cast<Instruction>(op)) {
                      if (Transforms.count(opIsInstr)) {
                        NewInstr->setOperand(idx, Transforms.lookup(opIsInstr));
                        insert = true;
                      }
                    }
                  }
                  (void)insert;
                  NewInstr->insertAfter(x);
                  Transforms.insert(std::pair<Instruction *, Instruction *>(
                      curDep2, NewInstr));
                  trans_new_instructions.push_back(NewInstr);
                  old_trans_new_instructions.push_back(curDep2);
                  x = NewInstr;
                }
              }
            }
            Type *I8 = Type::getInt8PtrTy(
                ((curLoad->getFunction())->getParent())->getContext());
            Type *I32 = Type::getInt32Ty((curLoad->getParent())->getContext());
            Function *PrefetchFunc = Intrinsic::getDeclaration(
                (curLoad->getFunction())->getParent(), Intrinsic::prefetch, I8);
            Instruction *oldGep = dyn_cast<Instruction>(last_gap);
            Instruction *gep = dyn_cast<Instruction>(Transforms.lookup(oldGep));
            Instruction *cast = dyn_cast<Instruction>(Builder.CreateBitCast(
                gep,
                Type::getInt8PtrTy(
                    ((curLoad->getFunction())->getParent())->getContext())));
            Value *ar[] = {cast, ConstantInt::get(I32, 0),
                           ConstantInt::get(I32, 3), ConstantInt::get(I32, 1)};
            CallInst *call = CallInst::Create(PrefetchFunc, ar);
            call->insertAfter(cast);
            x = call;
            (void)x;
          }
        }
      }
    } else {
      Instruction *InnerPhi = nullptr;
      Loop *LoadLoop = LI.getLoopFor(curLoad->getParent());
      SmallVector<Instruction *, 10> InnerPhis;
      for (int index = CapturedPhis.size() - 1; index >= 0; index--) {
        Loop *InnerPhiLoop = LI.getLoopFor(CapturedPhis[index]->getParent());
        if (InnerPhiLoop == LoadLoop) {
          InnerPhi = CapturedPhis[index];
          InnerPhis.push_back(InnerPhi);
          if (InjectPrefeches(curLoad, LI, InnerPhis, CapturedLoads,
                              CapturedInstrs, prefetchDist, true)) {
            done = true;
          }
        }
      }
    }
  }
  return done;
}

} // namespace llvm
