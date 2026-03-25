#pragma once

#include <llvm/Passes/PassBuilder.h>

namespace llvm {

class AFLGoTargetInjectionPass
    : public PassInfoMixin<AFLGoTargetInjectionPass> {

  class Target {
    std::string File;
    unsigned int Line;

  public:
    Target(std::string File, unsigned int Line) : File(File), Line(Line) {}

    bool matches(const DILocation &Loc);

    operator std::string() const { return File + ":" + std::to_string(Line); }
  };

  SmallVector<Target, 16> Targets;
  void parseTargets(std::unique_ptr<MemoryBuffer> &TargetsBuffer);

public:
  AFLGoTargetInjectionPass();

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

  static bool isRequired() { return true; }
};

} // namespace llvm