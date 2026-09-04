#include "CheckerReport.h"

#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/SuppressionManager.h"
#include "Fuzzing/TargetGeneration.h"

#include <algorithm>
#include <system_error>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace lotus::checker::tooling {

bool writeCheckerOutputAtomically(
    StringRef path, StringRef format,
    const std::function<void(raw_ostream &)> &write) {
  SmallString<256> temporaryPath;
  int temporaryFd = -1;
  if (std::error_code error = sys::fs::createUniqueFile(
          Twine(path) + ".tmp-%%%%%%", temporaryFd, temporaryPath)) {
    errs() << "Error writing " << format << " report: " << error.message()
           << "\n";
    return false;
  }

  raw_fd_ostream output(temporaryFd, true);
  write(output);
  output.close();
  if (output.has_error()) {
    const std::error_code error = output.error();
    output.clear_error();
    sys::fs::remove(temporaryPath);
    errs() << "Error writing " << format << " report: " << error.message()
           << "\n";
    return false;
  }

  if (std::error_code error = sys::fs::rename(temporaryPath, path)) {
    sys::fs::remove(temporaryPath);
    errs() << "Error writing " << format << " report: " << error.message()
           << "\n";
    return false;
  }
  return true;
}

bool validateReportOptions() {
  if (report_options::MinConfidenceScore < 0 ||
      report_options::MinConfidenceScore > 100) {
    errs() << "error: --report-min-score must be in [0,100]\n";
    return false;
  }
  return true;
}

int emitCheckerReports(BugReportMgr &manager,
                       const CheckerReportOptions &options) {
  if (!report_options::SuppressionFile.empty()) {
    SuppressionManager suppressions;
    if (!suppressions.loadFromFile(report_options::SuppressionFile)) {
      errs() << "Error loading suppressions from: "
             << report_options::SuppressionFile << "\n";
      return EXIT_ERROR;
    }
    manager.filterSuppressed(suppressions);
  }

  manager.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
  const BugReportMgr::ReportFilter filter{
      std::max(options.minScore, report_options::MinConfidenceScore.getValue()),
      report_options::ShowInvalidReports.getValue()};

  if (options.printText) {
    manager.print_detailed_reports(outs(), options.verbose, filter);
  }

  if (!report_options::TargetsOutputFile.empty()) {
    lotus::fuzzing::TargetGenerationOptions targetOptions;
    targetOptions.min_confidence_score = filter.minScore;
    targetOptions.include_invalid_reports = filter.includeInvalid;
    auto findings = lotus::fuzzing::collectFindings(manager, targetOptions);
    auto targets = lotus::fuzzing::collectTargets(findings);
    std::string errorMessage;
    if (!lotus::fuzzing::writeTargetsToFile(
            targets, report_options::TargetsOutputFile, &errorMessage)) {
      errs() << "Error writing fuzz targets: " << errorMessage << "\n";
      return EXIT_ERROR;
    }
    outs() << "\nFuzz targets written to: " << report_options::TargetsOutputFile
           << " (" << targets.size() << " targets)\n";
  }

  if (!report_options::JsonOutputFile.empty()) {
    if (!writeCheckerOutputAtomically(
            report_options::JsonOutputFile, "JSON", [&](raw_ostream &output) {
              manager.generate_json_report(output, filter);
            })) {
      return EXIT_ERROR;
    }
    outs() << "\nJSON report written to: " << report_options::JsonOutputFile
           << "\n";
  }

  if (!report_options::SarifOutputFile.empty()) {
    if (!writeCheckerOutputAtomically(
            report_options::SarifOutputFile, "SARIF", [&](raw_ostream &output) {
              manager.generate_sarif_report(output, filter);
            })) {
      return EXIT_ERROR;
    }
    outs() << "\nSARIF report written to: " << report_options::SarifOutputFile
           << "\n";
  }

  if (report_options::FailOnFindings &&
      manager.get_filtered_report_count(filter) > 0) {
    return EXIT_FINDINGS;
  }
  return EXIT_SUCCESS_CODE;
}

} // namespace lotus::checker::tooling
