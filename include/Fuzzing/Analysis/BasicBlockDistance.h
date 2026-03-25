#pragma once

#include "Fuzzing/Analysis/FunctionDistance.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/PassManager.h>

namespace llvm {

class AFLGoBasicBlockDistanceAnalysis
    : public AnalysisInfoMixin<AFLGoBasicBlockDistanceAnalysis> {
  bool UseExtendedCG;

public:
  static AnalysisKey Key;

  class Result {
  public:
    using FunctionToDistanceTy = AFLGoFunctionDistanceAnalysis::Result;
    using BBToDistanceTy = SmallDenseMap<BasicBlock *, double, 16>;
    using FunctionToOriginBBsMapTy = DenseMap<Function *, BBToDistanceTy>;

  private:
    const FunctionToDistanceTy FunctionToDistance;
    FunctionToOriginBBsMapTy FunctionToOriginBBs;

  public:
    explicit Result(FunctionToOriginBBsMapTy &FunctionToOriginBBs,
                    const FunctionToDistanceTy &FunctionToDistance)
        : FunctionToDistance(FunctionToDistance),
          FunctionToOriginBBs(FunctionToOriginBBs) {}

    BBToDistanceTy computeBBDistances(Function &F);
  };

  AFLGoBasicBlockDistanceAnalysis(bool UseExtendedCG)
      : UseExtendedCG(UseExtendedCG) {}

  Result run(Module &F, ModuleAnalysisManager &FAM);
};

} // namespace llvm