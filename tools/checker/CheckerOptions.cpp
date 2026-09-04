#include "CheckerOptions.h"

#include "Checker/Framework/BugReportMgr.h"

#include <algorithm>
#include <system_error>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <spdlog/spdlog.h>

namespace lotus::checker::tooling {

static llvm::cl::OptionCategory
    CommonCheckerCategory("Common Checker Options",
                          "Options shared by every Lotus checker engine");

llvm::cl::opt<std::string>
    Checks("checks",
           llvm::cl::desc("Comma-separated checker ids to run, or 'all'"),
           llvm::cl::value_desc("id[,id...]"), llvm::cl::init(""),
           llvm::cl::cat(CommonCheckerCategory),
           llvm::cl::sub(*llvm::cl::AllSubCommands));

llvm::cl::opt<bool> Verbose("verbose",
                            llvm::cl::desc("Print detailed finding traces"),
                            llvm::cl::init(false),
                            llvm::cl::cat(CommonCheckerCategory),
                            llvm::cl::sub(*llvm::cl::AllSubCommands));

static llvm::cl::opt<bool>
    AnalysisStats("analysis-stats", llvm::cl::desc("Print analysis statistics"),
                  llvm::cl::init(false), llvm::cl::cat(CommonCheckerCategory),
                  llvm::cl::sub(*llvm::cl::AllSubCommands));

static llvm::cl::opt<LogLevel> LoggingLevel(
    "log-level", llvm::cl::desc("Set engine diagnostic logging level"),
    llvm::cl::values(
        clEnumValN(LogLevel::None, "none", "Disable diagnostic logging"),
        clEnumValN(LogLevel::Error, "error", "Errors only"),
        clEnumValN(LogLevel::Warning, "warning", "Warnings and errors"),
        clEnumValN(LogLevel::Info, "info", "Informational messages"),
        clEnumValN(LogLevel::Debug, "debug", "Debug messages"),
        clEnumValN(LogLevel::Trace, "trace", "Trace messages")),
    llvm::cl::init(LogLevel::Warning), llvm::cl::cat(CommonCheckerCategory),
    llvm::cl::sub(*llvm::cl::AllSubCommands));

bool statsEnabled() { return AnalysisStats; }

LogLevel logLevel() { return LoggingLevel; }

bool logAtLeast(LogLevel level) {
  return static_cast<unsigned>(LoggingLevel.getValue()) >=
         static_cast<unsigned>(level);
}

void configureCommonLogging() {
  const spdlog::level::level_enum levels[] = {
      spdlog::level::off,  spdlog::level::err,   spdlog::level::warn,
      spdlog::level::info, spdlog::level::debug, spdlog::level::trace,
  };
  spdlog::set_level(levels[static_cast<unsigned>(LoggingLevel.getValue())]);
}

AnalysisStatsRecorder::AnalysisStatsRecorder(llvm::StringRef engine,
                                             const llvm::Module &module,
                                             BugReportMgr &reports)
    : engine_(engine.str()), module_(module), reports_(reports),
      initial_findings_(reports.get_total_reports()),
      start_(std::chrono::steady_clock::now()) {}

void AnalysisStatsRecorder::emit() const {
  if (!statsEnabled()) {
    return;
  }
  size_t instructionCount = 0;
  for (const llvm::Function &function : module_) {
    for (const llvm::Instruction &instruction : llvm::instructions(function)) {
      (void)instruction;
      ++instructionCount;
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start_);
  const int findings =
      std::max(0, reports_.get_total_reports() - initial_findings_);
  llvm::errs() << "Analysis statistics:\n"
               << "  engine: " << engine_ << "\n"
               << "  elapsed-ms: " << elapsed.count() << "\n"
               << "  functions: " << module_.size() << "\n"
               << "  instructions: " << instructionCount << "\n"
               << "  findings: " << findings << "\n";
}

bool hasExplicitCheckSelection() { return Checks.getNumOccurrences() != 0; }

llvm::Expected<std::vector<std::string>>
parseCheckSelection(llvm::StringRef scope) {
  if (!hasExplicitCheckSelection()) {
    return std::vector<std::string>{};
  }
  if (Checks.empty()) {
    return llvm::createStringError(std::errc::invalid_argument,
                                   "%s --checks value must not be empty",
                                   scope.str().c_str());
  }

  return parseCheckIdList(scope, Checks.getValue());
}

llvm::Expected<std::vector<std::string>>
parseCheckIdList(llvm::StringRef scope, llvm::StringRef csv, bool allowAll) {
  if (csv.empty()) {
    return llvm::createStringError(std::errc::invalid_argument,
                                   "%s check list must not be empty",
                                   scope.str().c_str());
  }

  llvm::SmallVector<llvm::StringRef, 8> pieces;
  csv.split(pieces, ',', -1, true);
  if (allowAll && pieces.size() == 1 && pieces.front().trim() == "all") {
    return std::vector<std::string>{"all"};
  }

  std::set<std::string> seen;
  std::vector<std::string> selected;
  selected.reserve(pieces.size());
  for (llvm::StringRef piece : pieces) {
    llvm::StringRef check = piece.trim();
    if (check.empty() || check == "all") {
      return llvm::createStringError(
          std::errc::invalid_argument,
          "invalid %s check list '%s'; 'all' must be used alone",
          scope.str().c_str(), csv.str().c_str());
    }
    if (!seen.insert(check.str()).second) {
      return llvm::createStringError(std::errc::invalid_argument,
                                     "duplicate %s check '%s'",
                                     scope.str().c_str(), check.str().c_str());
    }
    selected.push_back(check.str());
  }
  return selected;
}

llvm::Expected<std::set<std::string>> resolveChecks(EngineKind engine) {
  llvm::ArrayRef<NativeCheckDescriptor> supported =
      getBuiltinNativeChecks(engine);
  auto requested_or = parseCheckSelection(toString(engine));
  if (!requested_or) {
    return requested_or.takeError();
  }

  std::set<std::string> selected;
  if (!hasExplicitCheckSelection() ||
      (*requested_or == std::vector<std::string>{"all"})) {
    for (const NativeCheckDescriptor &check : supported) {
      if (hasExplicitCheckSelection() || check.default_enabled) {
        selected.insert(check.id.str());
      }
    }
    return selected;
  }

  for (const std::string &id : *requested_or) {
    const auto *match =
        llvm::find_if(supported, [&](const NativeCheckDescriptor &item) {
          return item.id == id;
        });
    if (match == supported.end()) {
      return llvm::createStringError(std::errc::invalid_argument,
                                     "unknown %s check '%s'", toString(engine),
                                     id.c_str());
    }
    selected.insert(id);
  }
  return selected;
}

} // namespace lotus::checker::tooling
