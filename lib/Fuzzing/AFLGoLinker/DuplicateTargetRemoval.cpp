#include "Fuzzing/AFLGoLinker/DuplicateTargetRemoval.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

PreservedAnalyses DuplicateTargetRemovalPass::run(Module &M,
                                                  ModuleAnalysisManager &AM) {
  PreservedAnalyses PA;
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (auto &F : M) {
    PA.intersect(run(F, FAM));
  }
  return PA;
}

PreservedAnalyses DuplicateTargetRemovalPass::run(Function &F,
                                                  FunctionAnalysisManager &AM) {

  SmallVector<CallInst *, 16> ToRemove;

  for (auto &BB : F) {
    bool BBHasTarget = false;

    for (auto &I : BB) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        auto *Fn = CI->getCalledFunction();
        if (Fn && Fn->getName().equals(
                      AFLGoTargetDetectionAnalysis::TargetFunctionName)) {
          if (BBHasTarget) {
            ToRemove.push_back(CI);
          }
          BBHasTarget = true;
        }
      }
    }
  }

  for (auto *CI : ToRemove) {
    CI->eraseFromParent();
  }

  if (ToRemove.empty()) {
    return PreservedAnalyses::all();
  }

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  return PA;
}