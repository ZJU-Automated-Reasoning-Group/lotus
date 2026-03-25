#pragma once

#include <memory>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/ProfileData/SampleProfReader.h"

namespace llvm {

struct SWPrefetchingLLVMPass : public FunctionPass {
  static char ID;
  Module *M = nullptr;

  SWPrefetchingLLVMPass() : FunctionPass(ID) {}

  bool doInitialization(Module &M) override;
  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  std::unique_ptr<sampleprof::SampleProfileReader> Reader;

  SmallVector<Instruction *, 10> IndirectLoads;
  SmallVector<Instruction *, 20> IndirectInstrs;
  SmallVector<Instruction *, 10> IndirectPhis;
  Instruction *IndirectLoad = nullptr;
  int64_t IndirectPrefetchDist = 0;

  bool SearchAlgorithm(Instruction *I, LoopInfo &LI, Instruction *&Phi,
                       SmallVector<Instruction *, 10> &Loads,
                       SmallVector<Instruction *, 20> &Instrs,
                       SmallVector<Instruction *, 10> &Phis);

  bool IsDep(Instruction *I, LoopInfo &LI, Instruction *&Phi,
             SmallVector<Instruction *, 10> &DependentLoads,
             SmallVector<Instruction *, 20> &DependentInstrs,
             SmallVector<Instruction *, 10> &DPhis);

  PHINode *getCanonicalishInductionVariable(Loop *L);
  bool CheckLoopCond(Loop *L);
  Instruction *GetIncomingValue(Loop *L, Instruction *curPN);
  ConstantInt *getValueAddedToIndVar(Loop *L, Instruction *nextInd);
  ConstantInt *getValueAddedToIndVarInLoopIterxxx(Loop *L);
  Value *getLoopEndCondxxx(Loop *L);
  CmpInst *getCompareInstrADD(Loop *L, Instruction *nextInd);
  CmpInst *getCompareInstrGetElememntPtr(Loop *L, Instruction *nextInd);

  bool InjectPrefeches(Instruction *curLoad, LoopInfo &LI,
                       SmallVector<Instruction *, 10> &CapturedPhis,
                       SmallVector<Instruction *, 10> &CapturedLoads,
                       SmallVector<Instruction *, 20> &CapturedInstrs,
                       int64_t prefetchDist, bool ItIsIndirectLoad);

  bool InjectPrefechesOnePhiPartOne(
      Instruction *curLoad, LoopInfo &LI,
      SmallVector<Instruction *, 10> &CapturedPhis,
      SmallVector<Instruction *, 10> &CapturedLoads,
      SmallVector<Instruction *, 20> &CapturedInstrs, int64_t prefetchDist,
      bool ItIsIndirectLoad);

  bool InjectPrefechesOnePhiPartTwo(Instruction *I, LoopInfo &LI,
                                    Instruction *Phi,
                                    SmallVector<Instruction *, 20> &DepInstrs,
                                    int64_t prefetchDist);
};

} // namespace llvm
