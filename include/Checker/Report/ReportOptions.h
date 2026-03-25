#ifndef CHECKER_REPORT_REPORTOPTIONS_H
#define CHECKER_REPORT_REPORTOPTIONS_H

#include <string>

#include <llvm/Support/CommandLine.h>

namespace report_options {

/**
 * Centralized options for bug report output formats.
 * These options apply to all checkers using BugReportMgr.
 *
 * Following the shared pattern: individual checkers should NOT
 * maintain their own output format options.
 */

// Category for output format options
extern llvm::cl::OptionCategory OutputCategory;

// Output format options (apply to all checkers)
extern llvm::cl::opt<std::string> JsonOutputFile;
extern llvm::cl::opt<std::string> SarifOutputFile;
extern llvm::cl::opt<std::string> TargetsOutputFile;
extern llvm::cl::opt<std::string> SuppressionFile;
extern llvm::cl::opt<int> MinConfidenceScore;
extern llvm::cl::opt<bool> ShowInvalidReports;

// Initialize options (call once at program startup)
void initializeReportOptions();

} // namespace report_options

#endif // CHECKER_REPORT_REPORTOPTIONS_H
