#pragma once

#include <llvm/Passes/PassBuilder.h>

namespace llvm {

class AFLGoTargetInjectionFixupPass
    : public PassInfoMixin<AFLGoTargetInjectionFixupPass> {

  uint32_t TargetCounter;

public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

  static bool isRequired() { return true; }
};

} // namespace llvm