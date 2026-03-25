#include "Fuzzing/AFLGoCompiler/TargetInjection.h"

#include <llvm/Passes/PassPlugin.h>

using namespace llvm;

llvm::PassPluginLibraryInfo getAFLGoCompilerPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "AFLGoCompiler", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel) {
                  MPM.addPass(AFLGoTargetInjectionPass());
                });

            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "instrument-compiler-aflgo") {
                    MPM.addPass(AFLGoTargetInjectionPass());
                    return true;
                  }

                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getAFLGoCompilerPluginInfo();
}