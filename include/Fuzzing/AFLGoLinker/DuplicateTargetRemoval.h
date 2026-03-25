#pragma once

#include <llvm/Passes/PassBuilder.h>

namespace llvm {

class DuplicateTargetRemovalPass
    : public PassInfoMixin<DuplicateTargetRemovalPass> {

public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm