#include "Fuzzing/AFLGoLinker/DAFL.h"
#include "Fuzzing/AFLGoLinker/DistanceInstrumentation.h"
#include "Fuzzing/AFLGoLinker/DuplicateTargetRemoval.h"
#include "Fuzzing/AFLGoLinker/FunctionDistanceInstrumentation.h"
#include "Fuzzing/AFLGoLinker/TargetInjectionFixup.h"

#include "Fuzzing/Analysis/BasicBlockDistance.h"
#include "Fuzzing/Analysis/DAFL.h"
#include "Fuzzing/Analysis/ExtendedCallGraph.h"
#include "Fuzzing/Analysis/FunctionDistance.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Transforms/Instrumentation.h>
#include <llvm/Transforms/Instrumentation/SanitizerCoverage.h>

using namespace llvm;

static cl::opt<bool> ClExtendCG(
    "extend-cg",
    cl::desc("Extend call graph with indirect edges through pointer analysis"),
    cl::init(false));

static cl::opt<bool>
    ClHawkeyeDistance("use-hawkeye-distance",
                      cl::desc("Use Hawkeye function distance definition"),
                      cl::init(false));

static cl::opt<bool>
    ClTraceFunctionDistance("trace-function-distance",
                            cl::desc("Add function distance tracing callbacks"),
                            cl::init(false));

static cl::opt<bool> ClDAFL("dafl", cl::desc("Enable DAFL instrumentation"),
                            cl::init(false));

static cl::opt<bool> ClDAFLNoTargetsNoError(
    "dafl-targets-no-error",
    cl::desc("Don't error out if DAFL targets are not found"), cl::init(false));

static cl::opt<bool>
    ClDAFLDebug("dafl-debug",
                cl::desc("Save debug files for DAFL instrumentation"),
                cl::init(false));

static cl::opt<bool>
    ClDAFLVerbose("dafl-verbose",
                  "Enable verbose output for DAFL instrumentation",
                  cl::init(false));

static cl::opt<std::string>
    ClDAFLInputFile("dafl-input-file",
                    cl::desc("Input file for DAFL analysis results"),
                    cl::value_desc("filename"));

static cl::opt<std::string>
    ClDAFLOutputFile("dafl-output-file",
                     cl::desc("Output file for DAFL analysis results"),
                     cl::value_desc("filename"));

static void addPasses(ModulePassManager &MPM) {
  MPM.addPass(DuplicateTargetRemovalPass());

  if (ClDAFL) {
    MPM.addPass(DAFLInstrumentationPass(ClDAFLOutputFile.getValue()));
  } else {
    if (ClTraceFunctionDistance) {
      MPM.addPass(FunctionDistancePass());
    }
    MPM.addPass(AFLGoDistanceInstrumentationPass());
  }

  SanitizerCoverageOptions Options;
  Options.CoverageType = SanitizerCoverageOptions::SCK_Edge;
  Options.TracePCGuard = true;
  Options.TraceCmp = true;
  MPM.addPass(ModuleSanitizerCoveragePass(Options));

  MPM.addPass(AFLGoTargetInjectionFixupPass());
}

llvm::PassPluginLibraryInfo getAFLGoLinkerPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "AFLGoLinker", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerAnalysisRegistrationCallback(
            [](FunctionAnalysisManager &FAM) {
              FAM.registerPass([] { return AFLGoTargetDetectionAnalysis(); });
            });
        PB.registerAnalysisRegistrationCallback([](ModuleAnalysisManager &MAM) {
          MAM.registerPass([] {
            return DAFLAnalysis(ClDAFLInputFile.getValue(), ClDAFLNoTargetsNoError,
                                ClDAFLDebug, ClDAFLVerbose);
          });
          MAM.registerPass([] { return ExtendedCallGraphAnalysis(); });
          MAM.registerPass([] {
            return AFLGoFunctionDistanceAnalysis(ClExtendCG, ClHawkeyeDistance);
          });
          MAM.registerPass(
              [] { return AFLGoBasicBlockDistanceAnalysis(ClExtendCG); });
        });

#if LLVM_VERSION_MAJOR >= 15
        PB.registerFullLinkTimeOptimizationLastEPCallback(
            [](ModulePassManager &MPM, OptimizationLevel) { addPasses(MPM); });
#endif

        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "instrument-linker-aflgo") {
                addPasses(MPM);
                return true;
              }

              return false;
            });
      }};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getAFLGoLinkerPluginInfo();
}