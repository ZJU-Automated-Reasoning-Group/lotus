
#pragma once

#include <llvm/Passes/PassBuilder.h>

namespace llvm {

class FunctionDistancePass
    : public PassInfoMixin<FunctionDistancePass> {

  uint32_t TargetCounter;

public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

  static bool isRequired() { return true; }
};

} // namespace llvm
