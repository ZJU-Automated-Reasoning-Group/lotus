#pragma once

#include <llvm/Support/CommandLine.h>

namespace kint {

// Define a category for performance options
extern llvm::cl::OptionCategory PerformanceCategory;

// Add a timeout option
extern llvm::cl::opt<unsigned> FunctionTimeout;
// Limit on path exploration per function (0 = no limit)
extern llvm::cl::opt<unsigned> MaxPathsPerFunction;

// Define a category for checker options
extern llvm::cl::OptionCategory CheckerCategory;

// Add command line options for enabling/disabling specific checkers
extern llvm::cl::opt<bool> CheckAll;
extern llvm::cl::opt<bool> CheckIntOverflow;
extern llvm::cl::opt<bool> CheckDivByZero;
extern llvm::cl::opt<bool> CheckBadShift;
extern llvm::cl::opt<bool> CheckArrayOOB;
extern llvm::cl::opt<bool> CheckDeadBranch;
extern llvm::cl::opt<bool> RobustReachability;
extern llvm::cl::opt<std::string> DumpEFConstraints;
extern llvm::cl::opt<bool> RobustUniversalUnknownLoads;
extern llvm::cl::opt<bool> RobustUniversalExternalGlobals;
extern llvm::cl::opt<bool> RobustUniversalInlineAsm;
extern llvm::cl::opt<std::string> RobustOnlyBugs;

// Define a category for logging options
extern llvm::cl::OptionCategory LoggingCategory;

// Define log levels
enum class LogLevel {
  DEBUG,   // Most verbose
  INFO,    // Normal informational messages
  WARNING, // Warnings
  ERROR,   // Errors
  NONE     // No logging
};

// Add logging control options
extern llvm::cl::opt<LogLevel> CurrentLogLevel;
extern llvm::cl::opt<bool> QuietLogging;
extern llvm::cl::opt<bool> StderrLogging;
extern llvm::cl::opt<std::string> LogFile;

// Initialize command line options
void initializeCommandLineOptions();

} // namespace kint
