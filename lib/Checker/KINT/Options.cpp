#include "Checker/KINT/Options.h"

#include "Checker/Framework/Subcommands.h"

namespace kint {

// Performance options
llvm::cl::OptionCategory
    PerformanceCategory("Performance Options",
                        "Options for controlling analysis performance");
llvm::cl::opt<unsigned>
    FunctionTimeout("kint.function-timeout-seconds",
                    llvm::cl::desc("Maximum time in seconds to spend analyzing "
                                   "a single function (0 = no limit)"),
                    llvm::cl::init(10), llvm::cl::cat(PerformanceCategory),
                    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<unsigned> MaxPathsPerFunction(
    "kint.max-paths-per-function",
    llvm::cl::desc(
        "Maximum number of path expansions per function (0 = no limit)"),
    llvm::cl::init(20000), llvm::cl::cat(PerformanceCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<unsigned> SummaryTimeout(
    "kint.summary-timeout-seconds",
    llvm::cl::desc("Maximum time in seconds to spend building a single "
                   "interprocedural summary (0 = no limit)"),
    llvm::cl::init(5), llvm::cl::cat(PerformanceCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<unsigned> SummaryMaxPathsPerFunction(
    "kint.summary-max-paths",
    llvm::cl::desc("Maximum number of path expansions while building a single "
                   "interprocedural summary (0 = no limit)"),
    llvm::cl::init(64), llvm::cl::cat(PerformanceCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<bool> AnalyzeAllFunctions(
    "kint.analyze-all-functions",
    llvm::cl::desc("Run SMT bug checks for all functions initialized by range "
                   "analysis instead of only taint/main entry points"),
    llvm::cl::init(false), llvm::cl::cat(PerformanceCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<SummaryMode> InterprocSummaryMode(
    "kint.summary-mode",
    llvm::cl::desc("Interprocedural summary application mode"),
    llvm::cl::values(
        clEnumValN(SummaryMode::Off, "off",
                   "Disable KINT interprocedural summaries"),
        clEnumValN(SummaryMode::On, "on",
                   "Build and apply KINT interprocedural summaries when "
                   "supported"),
        clEnumValN(SummaryMode::Required, "required",
                   "Attempt summaries aggressively and warn on fallback")),
    llvm::cl::init(SummaryMode::On), llvm::cl::cat(PerformanceCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));

// Checker configuration
llvm::cl::OptionCategory
    CheckerCategory("Bug Checker Options",
                    "Options specific to KINT bug checking");
bool CheckIntOverflow = false;
bool CheckDivByZero = false;
bool CheckBadShift = false;
bool CheckArrayOOB = false;
bool CheckDeadBranch = false;
llvm::cl::opt<bool> RobustReachability(
    "kint.robust-reachability",
    llvm::cl::desc("Enable robust reachability checks "
                   "(quantified SMT over unknown calls)"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<std::string> DumpEFConstraints(
    "kint.dump-ef-constraints",
    llvm::cl::desc("Append robust reachability (forall) constraints to file"),
    llvm::cl::value_desc("filename"), llvm::cl::init(""),
    llvm::cl::cat(CheckerCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<bool> RobustUniversalUnknownLoads(
    "kint.robust-universal-unknown-loads",
    llvm::cl::desc("Treat unknown loads as universally quantified variables"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<bool> RobustUniversalExternalGlobals(
    "kint.robust-universal-external-globals",
    llvm::cl::desc("Treat loads from external globals as universal variables"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<bool> RobustUniversalInlineAsm(
    "kint.robust-universal-inline-asm",
    llvm::cl::desc("Treat inline asm returns as universal variables"),
    llvm::cl::init(false), llvm::cl::cat(CheckerCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));
llvm::cl::opt<std::string> RobustChecks(
    "kint.robust-checks",
    llvm::cl::desc("Comma-separated check ids for robust reachability: "
                   "int-overflow,div-by-zero,bad-shift,array-oob,dead-branch. "
                   "Empty means the selected --checks set."),
    llvm::cl::value_desc("list"), llvm::cl::init(""),
    llvm::cl::cat(CheckerCategory),
    llvm::cl::sub(lotus::checker::tooling::kintSubCommand()));

void initializeCommandLineOptions() {
  // This function can be used to initialize any additional command line options
  // if needed in the future
}

} // namespace kint
