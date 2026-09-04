#include "Checker/Framework/ReportOptions.h"
namespace report_options {

// Output format category
llvm::cl::OptionCategory
    OutputCategory("Bug Report Output Options",
                   "Options for controlling bug report output formats (applies "
                   "to all checkers)");

// JSON output
llvm::cl::opt<std::string> JsonOutputFile(
    "report-json",
    llvm::cl::desc("Output bug reports in JSON format to the specified file"),
    llvm::cl::value_desc("filename"), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

// SARIF output
llvm::cl::opt<std::string> SarifOutputFile(
    "report-sarif",
    llvm::cl::desc("Output bug reports in SARIF format to the specified file"),
    llvm::cl::value_desc("filename"), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

// Directed fuzzing target output
llvm::cl::opt<std::string> TargetsOutputFile(
    "report-targets",
    llvm::cl::desc("Output ranked fuzz targets in file:line format to the "
                   "specified file"),
    llvm::cl::value_desc("filename"), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

// Suppression file
llvm::cl::opt<std::string> SuppressionFile(
    "suppressions", llvm::cl::desc("Path to suppression JSON file"),
    llvm::cl::value_desc("filename"), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

// Minimum confidence score filter
llvm::cl::opt<int> MinConfidenceScore(
    "report-min-score",
    llvm::cl::desc("Minimum confidence score for reported bugs (0-100)"),
    llvm::cl::init(0), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

// Show invalid reports
llvm::cl::opt<bool> ShowInvalidReports(
    "report-show-invalid",
    llvm::cl::desc("Include reports marked as invalid in output"),
    llvm::cl::init(false), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

llvm::cl::opt<bool> FailOnFindings(
    "fail-on-findings",
    llvm::cl::desc("Return exit code 1 when filtered findings are present"),
    llvm::cl::init(false), llvm::cl::cat(OutputCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

void initializeReportOptions() {
  // Currently nothing to initialize, but this function
  // is here for future extensibility
}

} // namespace report_options
