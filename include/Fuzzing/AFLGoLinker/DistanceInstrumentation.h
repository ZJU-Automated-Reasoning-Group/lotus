#pragma once

#include <llvm/Passes/PassBuilder.h>

namespace llvm {

class AFLGoDistanceInstrumentationPass
    : public PassInfoMixin<AFLGoDistanceInstrumentationPass> {

public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm