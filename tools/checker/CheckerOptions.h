#pragma once

#include "Checker/Framework/CheckerRegistry.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>

class BugReportMgr;
namespace llvm {
class Module;
} // namespace llvm

namespace lotus::checker::tooling {

class AnalysisStatsRecorder {
public:
  AnalysisStatsRecorder(llvm::StringRef engine, const llvm::Module &module,
                        BugReportMgr &reports);
  void emit() const;

private:
  std::string engine_;
  const llvm::Module &module_;
  BugReportMgr &reports_;
  int initial_findings_;
  std::chrono::steady_clock::time_point start_;
};

extern llvm::cl::opt<std::string> Checks;
extern llvm::cl::opt<bool> Verbose;

enum class LogLevel { None, Error, Warning, Info, Debug, Trace };

bool statsEnabled();
LogLevel logLevel();
bool logAtLeast(LogLevel level);
void configureCommonLogging();
bool hasExplicitCheckSelection();

llvm::Expected<std::vector<std::string>>
parseCheckSelection(llvm::StringRef scope);
llvm::Expected<std::vector<std::string>> parseCheckIdList(llvm::StringRef scope,
                                                          llvm::StringRef csv,
                                                          bool allowAll = true);

llvm::Expected<std::set<std::string>>
resolveChecks(lotus::checker::EngineKind engine);

} // namespace lotus::checker::tooling
