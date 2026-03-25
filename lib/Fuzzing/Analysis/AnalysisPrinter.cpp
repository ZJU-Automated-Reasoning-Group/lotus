#include "Fuzzing/Analysis/BasicBlockDistance.h"  
#include "Fuzzing/Analysis/DAFL.h"
#include "Fuzzing/Analysis/ExtendedCallGraph.h"
#include "Fuzzing/Analysis/FunctionDistance.h"
#include "Fuzzing/Analysis/TargetDetection.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/FormatVariadic.h>

using namespace llvm;

namespace {

static cl::opt<bool> ClExtendCG(
    "extend-cg",
    cl::desc("Extend call graph with indirect edges through pointer analysis"),
    cl::init(false));

static cl::opt<bool>
    ClHawkeyeDistance("use-hawkeye-distance",
                      cl::desc("Use Hawkeye function distance definition"),
                      cl::init(false));

static cl::opt<bool>
    ClDAFLDebug("dafl-debug",
                cl::desc("Save debug files for DAFL instrumentation"),
                cl::init(false));

static cl::opt<bool>
    ClDAFLVerbose("dafl-verbose",
                  "Enable verbose output for DAFL instrumentation",
                  cl::init(false));

class AFLGoFunctionDistancePrinterPass
    : public PassInfoMixin<AFLGoFunctionDistancePrinterPass> {
  raw_ostream &OS;

public:
  explicit AFLGoFunctionDistancePrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    auto &FunctionDistances = AM.getResult<AFLGoFunctionDistanceAnalysis>(M);

    OS << "function_name,distance\n";
    for (const auto &FunctionDistancePair : FunctionDistances) {
      auto *Function = FunctionDistancePair.first;
      auto Distance = FunctionDistancePair.second;
      OS << formatv("{0},{1}\n", Function->getName(), Distance);
    }

    return PreservedAnalyses::all();
  }
};

class AFLGoTargetDetectionPrinterPass
    : public PassInfoMixin<AFLGoTargetDetectionPrinterPass> {
  raw_ostream &OS;

public:
  explicit AFLGoTargetDetectionPrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    FunctionAnalysisManager &FAM =
        MAM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

    OS << "function_name,target_count\n";
    for (auto &F : M) {
      auto &Targets = FAM.getResult<AFLGoTargetDetectionAnalysis>(F);
      if (Targets.BBs.size()) {
        OS << formatv("{0},{1}\n", F.getName(), Targets.BBs.size());
      }
    }

    return PreservedAnalyses::all();
  }
};

class AFLGoBasicBlockDistancePrinterPass
    : public PassInfoMixin<AFLGoBasicBlockDistancePrinterPass> {
  raw_ostream &OS;

public:
  explicit AFLGoBasicBlockDistancePrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    auto BBDistanceResult = MAM.getResult<AFLGoBasicBlockDistanceAnalysis>(M);

    OS << "function_name,basic_block_name,distance\n";
    for (auto &F : M) {
      auto DistanceMap = BBDistanceResult.computeBBDistances(F);
      for (auto &DistanceEntry : DistanceMap) {
        auto *BB = DistanceEntry.first;
        auto Distance = DistanceEntry.second;

        OS << formatv("{0},", F.getName());
        BB->printAsOperand(OS, false);
        OS << formatv(",{0}\n", Distance);
      }
    }

    return PreservedAnalyses::all();
  }
};

class DAFLProximityPrinterPass
    : public PassInfoMixin<DAFLProximityPrinterPass> {
  raw_ostream &OS;

public:
  explicit DAFLProximityPrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
    auto &DAFL = MAM.getResult<DAFLAnalysis>(M);

    OS << "score,fn,bb\n";
    for (const auto &BBWeightPair : *DAFL) {
      auto *BB = BBWeightPair.first;
      auto Weight = BBWeightPair.second;
      DILocation *Loc;
      for (auto &I : *BB) {
        Loc = I.getDebugLoc().get();
        if (Loc && !Loc->getFilename().empty() && Loc->getLine() > 0) {
          break;
        }
      }
      OS << formatv("{0},{1},{2}:{3}\n", Weight, BB->getParent()->getName(),
                    Loc ? Loc->getFilename() : "n/a", Loc ? Loc->getLine() : 0);
    }

    return PreservedAnalyses::all();
  }
};

llvm::PassPluginLibraryInfo getAFLGoAnalysisPrinterPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "AFLGoAnalysisPrinter", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerAnalysisRegistrationCallback(
            [](FunctionAnalysisManager &FAM) {
              FAM.registerPass([] { return AFLGoTargetDetectionAnalysis(); });
            });

        PB.registerAnalysisRegistrationCallback([](ModuleAnalysisManager &MAM) {
          MAM.registerPass([] { return ExtendedCallGraphAnalysis(); });
          MAM.registerPass([] {
            return AFLGoFunctionDistanceAnalysis(ClExtendCG, ClHawkeyeDistance);
          });
          MAM.registerPass(
              [] { return AFLGoBasicBlockDistanceAnalysis(ClExtendCG); });
          MAM.registerPass([] {
            return DAFLAnalysis("", false, ClDAFLDebug, ClDAFLVerbose);
          });
        });

        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &MPM,
               ArrayRef<PassBuilder::PipelineElement>) {
              if (Name == "print-aflgo-target-detection") {
                MPM.addPass(AFLGoTargetDetectionPrinterPass(dbgs()));
                return true;
              }

              if (Name == "print-aflgo-function-distance") {
                MPM.addPass(AFLGoFunctionDistancePrinterPass(dbgs()));
                return true;
              }

              if (Name == "print-aflgo-basic-block-distance") {
                MPM.addPass(AFLGoBasicBlockDistancePrinterPass(dbgs()));
                return true;
              }

              if (Name == "print-dafl-proximity") {
                MPM.addPass(DAFLProximityPrinterPass(dbgs()));
                return true;
              }

              return false;
            });
      }};
}

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getAFLGoAnalysisPrinterPluginInfo();
}