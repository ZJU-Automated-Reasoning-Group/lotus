#pragma once

#include <llvm/IR/PassManager.h>

namespace llvm {

class AFLGoFunctionDistanceAnalysis
    : public AnalysisInfoMixin<AFLGoFunctionDistanceAnalysis> {
  bool UseExtendedCG;
  bool UseHawkeyeDistance;

public:
  static AnalysisKey Key;

  using Result = DenseMap<Function *, double>;

  AFLGoFunctionDistanceAnalysis(bool UseExtendedCG, bool UseHawkeyeDistance)
      : UseExtendedCG(UseExtendedCG), UseHawkeyeDistance(UseHawkeyeDistance) {}

  Result run(Module &M, ModuleAnalysisManager &MAM);
};

} // namespace llvm