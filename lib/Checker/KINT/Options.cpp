#include "Checker/KINT/Options.h"

namespace kint {

// Performance options
llvm::cl::OptionCategory
    PerformanceCategory("Performance Options",
                        "Options for controlling analysis performance");
llvm::cl::opt<unsigned>
    FunctionTimeout("function-timeout",
                    llvm::cl::desc("Maximum time in seconds to spend analyzing "
                                   "a single function (0 = no limit)"),
                    llvm::cl::init(10), llvm::cl::cat(PerformanceCategory));
llvm::cl::opt<unsigned> MaxPathsPerFunction(
    "max-paths-per-function",
    llvm::cl::desc(
        "Maximum number of path expansions per function (0 = no limit)"),
    llvm::cl::init(20000), llvm::cl::cat(PerformanceCategory));

// Checker options
llvm::cl::OptionCategory
    CheckerCategory("Bug Checker Options",
                    "Options for enabling/disabling specific bug checkers");
llvm::cl::opt<bool> CheckAll(
    "check-all",
    llvm::cl::desc("Enable all checkers (overrides individual settings)"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool>
    CheckIntOverflow("check-int-overflow",
                     llvm::cl::desc("Enable integer overflow checker"),
                     llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool>
    CheckDivByZero("check-div-by-zero",
                   llvm::cl::desc("Enable division by zero checker"),
                   llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool> CheckBadShift("check-bad-shift",
                                  llvm::cl::desc("Enable bad shift checker"),
                                  llvm::cl::init(false),
                                  llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool>
    CheckArrayOOB("check-array-oob",
                  llvm::cl::desc("Enable array index out of bounds checker"),
                  llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool>
    CheckDeadBranch("check-dead-branch",
                    llvm::cl::desc("Enable dead branch checker"),
                    llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool>
    RobustReachability("robust-reachability",
                       llvm::cl::desc("Enable robust reachability checks "
                                      "(quantified SMT over unknown calls)"),
                       llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<std::string> DumpEFConstraints(
    "dump-ef-constraints",
    llvm::cl::desc("Append robust reachability (forall) constraints to file"),
    llvm::cl::value_desc("filename"), llvm::cl::init(""),
    llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool> RobustUniversalUnknownLoads(
    "robust-universal-unknown-loads",
    llvm::cl::desc("Treat unknown loads as universally quantified variables"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool> RobustUniversalExternalGlobals(
    "robust-universal-external-globals",
    llvm::cl::desc("Treat loads from external globals as universal variables"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<bool> RobustUniversalInlineAsm(
    "robust-universal-inline-asm",
    llvm::cl::desc("Treat inline asm returns as universal variables"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory));
llvm::cl::opt<std::string> RobustOnlyBugs(
    "robust-only-bugs",
    llvm::cl::desc("Comma-separated bug classes for robust reachability "
                   "(overflow,div0,shift,oob,dead). Empty means all."),
    llvm::cl::value_desc("list"), llvm::cl::init(""),
    llvm::cl::cat(CheckerCategory));

// Define a category for logging options
llvm::cl::OptionCategory LoggingCategory("Logging Options",
                                         "Options for controlling log output");

// Add logging control options
llvm::cl::opt<LogLevel> CurrentLogLevel(
    "log-level", llvm::cl::desc("Set the logging level"),
    llvm::cl::values(
        clEnumValN(LogLevel::DEBUG, "debug",
                   "Display all messages including debug information"),
        clEnumValN(LogLevel::INFO, "info",
                   "Display per-value ranges and informational messages"),
        clEnumValN(LogLevel::WARNING, "warning",
                   "Display warnings and errors only (default)"),
        clEnumValN(LogLevel::ERROR, "error", "Display errors only"),
        clEnumValN(LogLevel::NONE, "none", "Suppress all log output")),
    llvm::cl::init(LogLevel::WARNING), llvm::cl::cat(LoggingCategory));

llvm::cl::opt<bool> QuietLogging(
    "quiet",
    llvm::cl::desc("Suppress most log output (equivalent to --log-level=none)"),
    llvm::cl::init(false), llvm::cl::cat(LoggingCategory));

llvm::cl::opt<bool>
    StderrLogging("log-to-stderr",
                  llvm::cl::desc("Redirect logs to stderr instead of stdout"),
                  llvm::cl::init(false), llvm::cl::cat(LoggingCategory));

llvm::cl::opt<std::string>
    LogFile("log-to-file",
            llvm::cl::desc("Redirect logs to the specified file"),
            llvm::cl::value_desc("filename"), llvm::cl::cat(LoggingCategory));

void initializeCommandLineOptions() {
  // This function can be used to initialize any additional command line options
  // if needed in the future
}

} // namespace kint
