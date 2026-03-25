#include "Checker/Concurrency/ConcurrencyChecker.h"
#include "Checker/Report/BugReportMgr.h"
#include "Checker/Report/ReportOptions.h"
#include "Checker/Report/SuppressionManager.h"
#include "Fuzzing/TargetGeneration.h"

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
    InputFilename(cl::Positional, cl::desc("<input file>"), cl::Required);
static cl::opt<std::string> ChecksList(
    "checks",
    cl::desc("Comma-separated checks to run: "
             "race,deadlock,atomicity,condvar,lock-mismatch,openmp,mpi "
             "(overrides individual flags)"),
    cl::value_desc("list"));
static cl::opt<bool> EnableDataRaces("check-data-races",
                                     cl::desc("Enable data race detection"),
                                     cl::init(true));
static cl::opt<bool> EnableDeadlocks("check-deadlocks",
                                     cl::desc("Enable deadlock detection"),
                                     cl::init(true));
static cl::opt<bool>
    EnableAtomicity("check-atomicity",
                    cl::desc("Enable atomicity violation detection"),
                    cl::init(true));
static cl::opt<bool>
    EnableCondVar("check-condvar",
                  cl::desc("Enable condition variable misuse detection"),
                  cl::init(true));
static cl::opt<bool> EnableLockMismatch(
    "check-lock-mismatch",
    cl::desc("Enable lock acquisition/release mismatch detection"),
    cl::init(true));
static cl::opt<bool>
    EnableOpenMP("check-openmp", cl::desc("Enable dedicated OpenMP bug checks"),
                 cl::init(true));
static cl::opt<bool> EnableMPI("check-mpi",
                               cl::desc("Enable dedicated MPI bug checks"),
                               cl::init(true));
static cl::opt<bool> AnalysisOnly(
    "analysis-only",
    cl::desc("Run analysis only (no bug checking), dump analysis results"),
    cl::init(false));
static cl::opt<std::string>
    AnalysisJsonOutput("analysis-json",
                       cl::desc("Output analysis results as JSON to specified "
                                "file (requires --analysis-only)"),
                       cl::value_desc("filename"));

int main(int argc, char **argv) {
  // Initialize centralized report options
  report_options::initializeReportOptions();

  cl::ParseCommandLineOptions(
      argc, argv,
      "Concurrency Checker Tool\n"
      "  Use --report-json=<file> or --report-sarif=<file> for output\n");

  // Parse the input LLVM IR file
  SMDiagnostic err;
  LLVMContext context;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);

  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }

  outs() << "Analyzing module: " << module->getModuleIdentifier() << "\n";

  ConcurrencyChecker checker(*module);

  // Config-driven activation: --checks=race,deadlock,... overrides individual
  // flags (Goblint-style)
  if (!ChecksList.empty()) {
    const std::string &list = ChecksList.getValue();
    checker.enableDataRaceCheck(list.find("race") != std::string::npos);
    checker.enableDeadlockCheck(list.find("deadlock") != std::string::npos);
    checker.enableAtomicityCheck(list.find("atomicity") != std::string::npos);
    checker.enableCondVarCheck(list.find("condvar") != std::string::npos);
    checker.enableLockMismatchCheck(list.find("lock-mismatch") !=
                                    std::string::npos);
    checker.enableOpenMPCheck(list.find("openmp") != std::string::npos);
    checker.enableMPICheck(list.find("mpi") != std::string::npos);
  } else {
    checker.enableDataRaceCheck(EnableDataRaces);
    checker.enableDeadlockCheck(EnableDeadlocks);
    checker.enableAtomicityCheck(EnableAtomicity);
    checker.enableCondVarCheck(EnableCondVar);
    checker.enableLockMismatchCheck(EnableLockMismatch);
    checker.enableOpenMPCheck(EnableOpenMP);
    checker.enableMPICheck(EnableMPI);
  }

  if (AnalysisOnly) {
    checker.enableDataRaceCheck(true);
    checker.enableDeadlockCheck(true);
    checker.enableAtomicityCheck(true);
    checker.enableCondVarCheck(true);
    checker.enableLockMismatchCheck(true);
    checker.enableOpenMPCheck(true);
    checker.enableMPICheck(true);
  }

  checker.runAnalyses();

  if (AnalysisOnly) {
    outs() << "Running concurrency analyses (analysis-only mode)...\n";
    if (!AnalysisJsonOutput.empty()) {
      // Output to JSON file
      std::error_code EC;
      raw_fd_ostream json_out(AnalysisJsonOutput, EC, sys::fs::OF_None);
      if (!EC) {
        checker.dumpAnalysisResults(json_out, true);
        outs() << "\nAnalysis results written to JSON: " << AnalysisJsonOutput
               << "\n";
      } else {
        errs() << "Error writing analysis JSON: " << EC.message() << "\n";
        return 1;
      }
    } else {
      // Output to stdout in human-readable format
      checker.dumpAnalysisResults(outs(), false);
    }
    return 0;
  }

  outs() << "Running concurrency checks...\n";
  checker.runChecks();

  // Print analysis statistics
  auto stats = checker.getStatistics();
  outs() << "\n=== Concurrency Analysis Statistics ===\n";
  outs() << "Total Instructions: " << stats.totalInstructions << "\n";
  outs() << "MHP Pairs: " << stats.mhpPairs << "\n";
  outs() << "Locks Analyzed: " << stats.locksAnalyzed << "\n";
  outs() << "Data Races Found: " << stats.dataRacesFound << "\n";
  outs() << "Deadlocks Found: " << stats.deadlocksFound << "\n";
  outs() << "Atomicity Violations Found: " << stats.atomicityViolationsFound
         << "\n";
  outs() << "Cond Var Bugs Found: " << stats.condVarBugsFound << "\n";
  outs() << "Lock Mismatches Found: " << stats.lockMismatchesFound << "\n";
  outs() << "OpenMP Bugs Found: " << stats.openMPBugsFound << "\n";
  outs() << "  OpenMP Tasks Tracked: " << stats.openMPSummary.task_count
         << "\n";
  outs() << "  OpenMP Tasks With Dependencies: "
         << stats.openMPSummary.task_with_dependencies_count << "\n";
  outs() << "  OpenMP Included Tasks: "
         << stats.openMPSummary.included_task_count << "\n";
  outs() << "  OpenMP Taskloops: " << stats.openMPSummary.taskloop_count
         << "\n";
  outs() << "  OpenMP Wait Boundaries: "
         << stats.openMPSummary.wait_boundary_count << "\n";
  outs() << "  OpenMP Partial Wait Boundaries: "
         << stats.openMPSummary.partial_wait_boundary_count << "\n";
  outs() << "  OpenMP Taskgroups: "
         << stats.openMPSummary.taskgroup_region_count << "\n";
  outs() << "  OpenMP Worksharing Regions: "
         << stats.openMPSummary.single_region_count +
                stats.openMPSummary.sections_region_count +
                stats.openMPSummary.worksharing_loop_count +
                stats.openMPSummary.reduction_region_count +
                stats.openMPSummary.ordered_region_count +
                stats.openMPSummary.master_region_count
         << "\n";
  outs() << "  OpenMP Atomic Regions: "
         << stats.openMPSummary.atomic_region_count << "\n";
  outs() << "  OpenMP Flushes: " << stats.openMPSummary.flush_count << "\n";
  outs() << "  OpenMP Cancels: " << stats.openMPSummary.cancel_count << "\n";
  outs() << "  OpenMP Target Regions: "
         << stats.openMPSummary.target_region_count +
                stats.openMPSummary.target_data_region_count
         << "\n";
  outs() << "MPI Bugs Found: " << stats.mpiBugsFound << "\n";
  outs() << "  MPI Operations Tracked: " << stats.mpiSummary.operation_count
         << "\n";
  outs() << "  MPI Nonblocking Operations: "
         << stats.mpiSummary.nonblocking_operation_count << "\n";
  outs() << "  MPI Collective Operations: "
         << stats.mpiSummary.collective_operation_count << "\n";
  outs() << "  MPI Communicator Management Ops: "
         << stats.mpiSummary.communicator_management_count << "\n";
  outs() << "  MPI Request Management Ops: "
         << stats.mpiSummary.request_management_count << "\n";
  outs() << "  MPI RMA Data Ops: " << stats.mpiSummary.rma_operation_count
         << "\n";
  outs() << "  MPI RMA Sync Ops: " << stats.mpiSummary.rma_sync_count << "\n";
  outs() << "  MPI Unsynchronized RMA Ops: "
         << stats.mpiSummary.unsynchronized_rma_count << "\n";
  outs() << "  MPI RMA Races: " << stats.mpiSummary.rma_race_count << "\n";
  outs() << "  MPI Leaked Windows: " << stats.mpiSummary.leaked_window_count
         << "\n";
  outs() << "  MPI Collective Slots Tracked: "
         << stats.mpiSummary.collective_slot_count << "\n";

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

  // 3. Print bug report summary (shared pattern - applies to all checkers)
  mgr.print_summary(outs());

  // 4. Handle centralized output formats (applies to all checkers)
  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions options;
    options.min_confidence_score = report_options::MinConfidenceScore;
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

  if (!report_options::JsonOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream json_out(report_options::JsonOutputFile, EC,
                            sys::fs::OF_None);
    if (!EC) {
      mgr.generate_json_report(json_out, report_options::MinConfidenceScore);
      outs() << "\nJSON report written to: " << report_options::JsonOutputFile
             << "\n";
    } else {
      errs() << "Error writing JSON report: " << EC.message() << "\n";
    }
  }

  // 5. Generate SARIF report if requested
  if (!report_options::SarifOutputFile.empty()) {
    std::error_code EC;
    raw_fd_ostream sarif_out(report_options::SarifOutputFile, EC,
                             sys::fs::OF_None);
    if (!EC) {
      mgr.generate_sarif_report(sarif_out, report_options::MinConfidenceScore);
      outs() << "\nSARIF report written to: " << report_options::SarifOutputFile
             << "\n";
    } else {
      errs() << "Error writing SARIF report: " << EC.message() << "\n";
    }
  }

  size_t total_bugs = stats.dataRacesFound + stats.deadlocksFound +
                      stats.atomicityViolationsFound + stats.condVarBugsFound +
                      stats.lockMismatchesFound + stats.openMPBugsFound +
                      stats.mpiBugsFound;
  return total_bugs > 0 ? 1 : 0;
}
