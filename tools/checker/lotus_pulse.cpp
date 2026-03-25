/*
 * Pulse Checker Tool
 *
 * A bug finder using biabductive analysis, inspired by Infer's Pulse.
 * Uses UnderApproxAA for must-alias canonicalization.
 */

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Report/PulseLogger.h"
#include "Checker/Pulse/Report/PulseOptions.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Fuzzing/TargetGeneration.h"

#include <memory>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace pulse;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required);
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<std::string>
    LogLevelOpt("log-level",
                cl::desc("Log level: none, error, warning, info, debug, trace"),
                cl::init("info"));
static cl::opt<bool> ShowPulseStats("pulse-stats",
                                    cl::desc("Show Pulse analysis statistics"),
                                    cl::init(true));
static cl::opt<std::string> JsonOutput("json-output",
                                       cl::desc("Output JSON report to file"),
                                       cl::init(""));
static cl::opt<int>
    MinScore("min-score",
             cl::desc("Minimum confidence score for reporting (0-100)"),
             cl::init(0));
static cl::opt<bool> NoSMT("no-smt",
                           cl::desc("Disable SMT solving (fast mode); do not "
                                    "query Z3 for path satisfiability"),
                           cl::init(false));

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Pulse Bug Finder\n");

  // Configure logging
  pulse::LogLevel level = pulse::LogLevel::Info;
  if (LogLevelOpt == "none")
    level = pulse::LogLevel::None;
  else if (LogLevelOpt == "error")
    level = pulse::LogLevel::Error;
  else if (LogLevelOpt == "warning")
    level = pulse::LogLevel::Warning;
  else if (LogLevelOpt == "info")
    level = pulse::LogLevel::Info;
  else if (LogLevelOpt == "debug")
    level = pulse::LogLevel::Debug;
  else if (LogLevelOpt == "trace")
    level = pulse::LogLevel::Trace;

  if (Verbose && level < pulse::LogLevel::Debug) {
    level = pulse::LogLevel::Debug;
  }

  PulseLogger::setLevel(level);
  PulseLogger::setOutputStream(&errs());
  PulseLogger::resetStats();

  pulse::options::setDisableSMT(NoSMT);
  if (NoSMT) {
    PulseLogger::info("Fast mode: SMT solving disabled");
  }

  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  PulseLogger::info("Starting Pulse analysis");
  PulseLogger::info("Module: " + M->getName().str());
  PulseLogger::startTimer("total_analysis");

  auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
      *M, lotus::AAConfig::UnderApprox());
  PulseChecker checker(M.get(), AA.get());

  PulseLogger::startTimer("analysis");
  checker.analyze();
  PulseLogger::endTimer("analysis");

  PulseLogger::endTimer("total_analysis");

  if (ShowPulseStats) {
    PulseLogger::printStats();
  }

  // Post-processing: Suppression and Deduplication
  BugReportMgr &mgr = BugReportMgr::get_instance();

  // 1. Load and apply suppressions
  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager suppMgr;
    if (suppMgr.loadFromFile(report_options::SuppressionFile)) {
      mgr.setSuppressionManager(&suppMgr);
      mgr.filterSuppressed();
      auto stats = suppMgr.getStats();
      outs() << "\nApplied suppressions: " << stats.totalSuppressions
             << " across " << stats.totalFiles << " files\n";
    } else {
      errs() << "Warning: Could not load suppressions from: "
             << report_options::SuppressionFile << "\n";
    }
  }

  // 2. Final deduplication (enhanced algorithm)
  mgr.deduplicate_reports(true);

  // 3. Print bug summary
  if (mgr.get_total_reports() > 0) {
    outs() << "\n";
    mgr.print_summary(outs());
  }

  // 4. Generate JSON report if requested
  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions options;
    options.min_confidence_score =
        std::max(MinScore, report_options::MinConfidenceScore);
    options.include_invalid_reports = report_options::ShowInvalidReports;
    auto findings = lotus::fuzzing::collectFindings(mgr, options);
    auto targets = lotus::fuzzing::collectTargets(findings);

    std::string errorMessage;
    if (!lotus::fuzzing::writeTargetsToFile(targets,
                                            report_options::TargetsOutputFile,
                                            &errorMessage)) {
      errs() << "Error writing fuzz targets: " << errorMessage << "\n";
      return 1;
    }
    outs() << "\nFuzz targets written to: " << report_options::TargetsOutputFile
           << " (" << targets.size() << " targets)\n";
  }

  if (!JsonOutput.empty()) {
    std::error_code EC;
    raw_fd_ostream json_out(JsonOutput, EC);
    if (EC) {
      errs() << "Error opening JSON output file: " << EC.message() << "\n";
      return 1;
    }
    mgr.generate_json_report(json_out, MinScore);
    json_out.close();
    outs() << "\nJSON report written to: " << JsonOutput << "\n";
  }

  // 5. Generate SARIF report if requested
  if (!report_options::SarifOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream sarif_out(report_options::SarifOutputFile, EC);
    if (EC) {
      errs() << "Error opening SARIF output file: " << EC.message() << "\n";
      return 1;
    }
    mgr.generate_sarif_report(
        sarif_out, std::max(MinScore, report_options::MinConfidenceScore));
    sarif_out.close();
    outs() << "\nSARIF report written to: " << report_options::SarifOutputFile
           << "\n";
  }

  PulseLogger::info("Analysis complete");
  return 0;
}
