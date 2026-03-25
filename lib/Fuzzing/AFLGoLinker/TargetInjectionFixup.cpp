#include "Fuzzing/AFLGoLinker/TargetInjectionFixup.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/PassManager.h>

using namespace llvm;

PreservedAnalyses
AFLGoTargetInjectionFixupPass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto &C = M.getContext();

  FunctionAnalysisManager &FAM =
      MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  for (auto &F : M) {
    if (F.isDeclaration()) {
      continue;
    }

    auto &Targets = FAM.getResult<AFLGoTargetDetectionAnalysis>(F);
    for (auto &TargetPair : Targets.BBs) {
      auto *CB = TargetPair.second;
      auto *Arg = CB->getArgOperand(0);
      auto *ArgType = cast<IntegerType>(Arg->getType());
      auto *NewArg = ConstantInt::get(ArgType, TargetCounter);

      CB->setArgOperand(0, NewArg);

      TargetCounter++;
    }
  }

  PreservedAnalyses PA;
  PA.preserve<AFLGoTargetDetectionAnalysis>();
  return PA;
}