#include "Checker/Concurrency/ConcurrencyChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Checker/Tooling/CheckerSubcommands.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"

#include <cstddef>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace concurrency;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::Required,
                  cl::sub(lotus::checker::tooling::concurrencySubCommand()));
enum class RunMode { Analysis, Check };
static cl::opt<RunMode>
    Mode("concur.mode", cl::desc("Concurrency engine run mode"),
         cl::values(clEnumValN(RunMode::Analysis, "analysis",
                               "Run analyses and emit analysis facts"),
                    clEnumValN(RunMode::Check, "check",
                               "Run analyses and bug checkers")),
         cl::init(RunMode::Check),
         cl::sub(lotus::checker::tooling::concurrencySubCommand()));
enum class OutputFormat { Json, Text };
static cl::opt<OutputFormat> OutputFormatOpt(
    "concur.output-format", cl::desc("Analysis output format"),
    cl::values(clEnumValN(OutputFormat::Json, "json", "JSON output"),
               clEnumValN(OutputFormat::Text, "text", "Human-readable text")),
    cl::init(OutputFormat::Text),
    cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<std::string>
    Output("concur.output", cl::desc("Write analysis output to this file"),
           cl::value_desc("filename"), cl::init(""),
           cl::sub(lotus::checker::tooling::concurrencySubCommand()));
static cl::opt<bool> SparseFlowSensitive(
    "concur.sparse-flow-sensitive",
    cl::desc("Refine data-race alias pairs with a thread-aware sparse "
             "flow-sensitive points-to solve"),
    cl::init(false),
    cl::sub(lotus::checker::tooling::concurrencySubCommand()));

int runConcurrencyCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  if (Mode == RunMode::Check && (Output.getNumOccurrences() != 0 ||
                                 OutputFormatOpt.getNumOccurrences() != 0)) {
    errs() << "error: --concur.output and --concur.output-format require "
              "--concur.mode=analysis\n";
    return lotus::checker::tooling::EXIT_ERROR;
  }

  // Parse the input LLVM IR file
  SMDiagnostic err;
  LLVMContext context;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);

  if (!module) {
    err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("concur", *module, mgr);

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Info))
    outs() << "Analyzing module: " << module->getModuleIdentifier() << "\n";

  ConcurrencyChecker checker(*module);

  auto selectedOr = lotus::checker::tooling::resolveChecks(
      lotus::checker::EngineKind::Concurrency);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const auto &selected = *selectedOr;
  checker.enableDataRaceCheck(selected.count("data-race"));
  checker.enableDeadlockCheck(selected.count("deadlock"));
  checker.enableAtomicityCheck(selected.count("atomicity"));
  checker.enableCondVarCheck(selected.count("condvar"));
  checker.enableLockMismatchCheck(selected.count("lock-mismatch"));
  checker.enableOpenMPCheck(selected.count("openmp"));
  checker.enableMPICheck(selected.count("mpi"));
  checker.enableCUDACheck(selected.count("cuda"));
  checker.enableSparseFlowSensitiveRefinement(SparseFlowSensitive);

  checker.runAnalyses();

  if (Mode == RunMode::Analysis) {
    if (lotus::checker::tooling::logAtLeast(
            lotus::checker::tooling::LogLevel::Info))
      outs() << "Running concurrency analyses...\n";
    const bool json = OutputFormatOpt == OutputFormat::Json;
    if (!Output.empty()) {
      if (!lotus::checker::tooling::writeCheckerOutputAtomically(
              Output, "concurrency analysis", [&](raw_ostream &stream) {
                checker.dumpAnalysisResults(stream, json);
              })) {
        return lotus::checker::tooling::EXIT_ERROR;
      }
      if (lotus::checker::tooling::logAtLeast(
              lotus::checker::tooling::LogLevel::Info))
        outs() << "Analysis results written to: " << Output << "\n";
    } else {
      checker.dumpAnalysisResults(outs(), json);
    }
    stats.emit();
    return lotus::checker::tooling::EXIT_SUCCESS_CODE;
  }

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Info))
    outs() << "Running concurrency checks...\n";
  checker.runChecks();

  stats.emit();
  return lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});
}
