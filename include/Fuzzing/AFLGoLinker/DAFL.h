#pragma once

#include <llvm/Passes/PassBuilder.h>

namespace llvm {

class DAFLInstrumentationPass : public PassInfoMixin<DAFLInstrumentationPass> {

  std::string OutputFile;

public:
  DAFLInstrumentationPass(std::string OutputFile) : OutputFile(OutputFile) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  static bool isRequired() { return true; }
};

} // namespace llvm