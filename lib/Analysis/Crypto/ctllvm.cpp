/*
 * Public pass entrypoints and plugin registration for the ctllvm pass.
 */

#include "Analysis/Crypto/ctllvm.h"

#include "CTInternal.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <utility>

using ctllvm::detail::CryptoAnalysisImpl;

CTPass::CTPass() = default;

CTPass::CTPass(CTOptions options) : options_(std::move(options)) {}

llvm::PreservedAnalyses CTPass::run(llvm::Module &M,
                                    llvm::ModuleAnalysisManager &MAM) {
  CryptoAnalysisImpl impl(options_);
  return impl.run(M, MAM);
}

llvm::PassPluginLibraryInfo getPassPluginInfo() {
  const auto callback = [](llvm::PassBuilder &PB) {
    PB.registerPipelineStartEPCallback(
        [](llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level) {
          MPM.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::PromotePass()));
          return true;
        });

    PB.registerOptimizerLastEPCallback(
#if LLVM_VERSION_MAJOR < 20
        [&](llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level) {
#else
        [&](llvm::ModulePassManager &MPM, llvm::OptimizationLevel Level,
            llvm::ThinOrFullLTOPhase Phase) {
#endif
          MPM.addPass(CTPass());
          return true;
        });

    PB.registerPipelineParsingCallback(
        [](llvm::StringRef Name, llvm::ModulePassManager &MPM,
           llvm::ArrayRef<llvm::PassBuilder::PipelineElement>) {
          if (Name == "ctllvm") {
            MPM.addPass(CTPass());
            return true;
          }
          return false;
        });
  };

  return {LLVM_PLUGIN_API_VERSION, "CTPass", "0.0.1", callback};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getPassPluginInfo();
}
