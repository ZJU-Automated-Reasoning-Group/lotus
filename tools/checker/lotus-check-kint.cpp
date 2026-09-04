// Kint: A Bug-Finding Tool for C Programs (Refactored version)

#include "Checker/KINT/Log.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "Checker/KINT/MKintPass.h"
#include "Checker/KINT/Options.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"

#include <llvm/IR/PassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

using namespace llvm;

// Command line options
static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<IR file>"), cl::Required,
                  cl::sub(lotus::checker::tooling::kintSubCommand()));
static void buildKintPipeline(ModulePassManager &MPM) {
  MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
  MPM.addPass(createModuleToFunctionPassAdaptor(SROAPass()));
  MPM.addPass(kint::MKintPass());
}

// registering pass (new pass manager).
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MKintPass", "v0.1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "mkint-pass") {
                    buildKintPipeline(MPM);
                    return true;
                  }
                  return false;
                });
          }};
}

int runKintCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  // Initialize command line options
  kint::initializeCommandLineOptions();

  // Configure the logger
  mkint::LogConfig logConfig;
  logConfig.useStderr = true;

  switch (lotus::checker::tooling::logLevel()) {
  case lotus::checker::tooling::LogLevel::Trace:
  case lotus::checker::tooling::LogLevel::Debug:
    logConfig.logLevel = mkint::LogLevel::DEBUG;
    break;
  case lotus::checker::tooling::LogLevel::Info:
    logConfig.logLevel = mkint::LogLevel::INFO;
    break;
  case lotus::checker::tooling::LogLevel::Warning:
    logConfig.logLevel = mkint::LogLevel::WARNING;
    break;
  case lotus::checker::tooling::LogLevel::Error:
    logConfig.logLevel = mkint::LogLevel::ERROR;
    break;
  case lotus::checker::tooling::LogLevel::None:
    logConfig.logLevel = mkint::LogLevel::NONE;
    logConfig.quiet = true;
    break;
  }

  mkint::Logger::getInstance().configure(logConfig);

  auto selectedOr =
      lotus::checker::tooling::resolveChecks(lotus::checker::EngineKind::KINT);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const auto &selected = *selectedOr;
  kint::CheckIntOverflow = selected.count("int-overflow");
  kint::CheckDivByZero = selected.count("div-by-zero");
  kint::CheckBadShift = selected.count("bad-shift");
  kint::CheckArrayOOB = selected.count("array-oob");
  kint::CheckDeadBranch = selected.count("dead-branch");

  if (kint::RobustChecks.empty()) {
    std::string robustChecks;
    for (const std::string &id : selected) {
      if (!robustChecks.empty()) {
        robustChecks += ',';
      }
      robustChecks += id;
    }
    kint::RobustChecks = robustChecks;
  } else {
    auto robustOr = lotus::checker::tooling::parseCheckIdList(
        "KINT robust", kint::RobustChecks, false);
    if (!robustOr) {
      logAllUnhandledErrors(robustOr.takeError(), errs(), "error: ");
      return lotus::checker::tooling::EXIT_ERROR;
    }
    for (const std::string &id : *robustOr) {
      if (!selected.count(id)) {
        errs() << "error: KINT robust check '" << id
               << "' is not enabled by --checks\n";
        return lotus::checker::tooling::EXIT_ERROR;
      }
    }
  }

  // Print checker configuration
  MKINT_LOG() << "Checker Configuration:";
  MKINT_LOG() << "  Integer Overflow: "
              << (kint::CheckIntOverflow ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Division by Zero: "
              << (kint::CheckDivByZero ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Bad Shift: "
              << (kint::CheckBadShift ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Array Out of Bounds: "
              << (kint::CheckArrayOOB ? "Enabled" : "Disabled");
  MKINT_LOG() << "  Dead Branch: "
              << (kint::CheckDeadBranch ? "Enabled" : "Disabled");

  // Add performance configuration information
  MKINT_LOG() << "Performance Configuration:";
  MKINT_LOG() << "  Function Timeout: "
              << (kint::FunctionTimeout == 0
                      ? "No limit"
                      : std::to_string(kint::FunctionTimeout) + " seconds");

  // Explicitly selecting an empty checker set is a configuration error.
  if (!kint::CheckIntOverflow && !kint::CheckDivByZero &&
      !kint::CheckBadShift && !kint::CheckArrayOOB && !kint::CheckDeadBranch) {
    errs() << "error: no KINT checks selected\n"
           << "hint: select at least one checker with --checks=<id>\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Load the module to analyze
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Err;
  std::unique_ptr<llvm::Module> M;

  M = llvm::parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, llvm::errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("kint", *M, mgr);

  // Create and run the pass (new pass manager with cross-registered proxies)
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  llvm::ModulePassManager MPM;
  buildKintPipeline(MPM);

  // Run analysis pipeline (bugs are automatically reported to BugReportMgr)
  MPM.run(*M, MAM);
  stats.emit();
  return lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});
}
