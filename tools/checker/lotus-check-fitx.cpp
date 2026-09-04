/*
 * FiTx Bug Finder Tool (Standalone)
 *
 * FiTx: Framework for Finger Traceable Bugs in Linux
 * A static analysis framework for detecting common memory and concurrency bugs.
 *
 * Supported bug types:
 * - Double Free (df)
 * - Double Lock (dl)
 * - Double Unlock (dul)
 * - Memory Leak (leak)
 * - Null Pointer Dereference (nullptr)
 * - Use After Free (uaf)
 * - Use Before Initialization (ubi)
 * - Reference Count Issues (ref_count, ref_uncount)
 *
 * Usage: lotus-check --engine=fitx [options] <input bitcode>
 */

#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Core/Utils.h"
#include "Checker/FiTx/Detector/DF_Detector.h"
#include "Checker/FiTx/Detector/DL_Detector.h"
#include "Checker/FiTx/Detector/DUL_Detector.h"
#include "Checker/FiTx/Detector/Leak_Detector.h"
#include "Checker/FiTx/Detector/NullPtr_Detector.h"
#include "Checker/FiTx/Detector/Ref_Detector.h"
#include "Checker/FiTx/Detector/UAF_Detector.h"
#include "Checker/FiTx/Detector/Unref_Detector.h"
#include "Checker/FiTx/Detector/UseBeforeInit_Detector.h"
#include "Checker/FiTx/Framework_IR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"

#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required,
              cl::sub(lotus::checker::tooling::fitxSubCommand()));
namespace {
using DetectorDefinition = void (*)(fitx::StateManager &);
using DetectorDefinitions = std::vector<DetectorDefinition>;

class SelectedDetector final : public fitx::FrameworkPass {
public:
  explicit SelectedDetector(DetectorDefinitions definitions)
      : definitions_(std::move(definitions)) {}

  void defineStates() override {
    for (DetectorDefinition define : definitions_) {
      fitx::StateManager manager;
      define(manager);
      addStateManager(std::move(manager));
    }
  }

private:
  DetectorDefinitions definitions_;
};

const StringMap<DetectorDefinitions> &detectorRegistry() {
  static const StringMap<DetectorDefinitions> registry = [] {
    StringMap<DetectorDefinitions> result;
    result["double-free"] = {DoubleFree::define_states};
    result["double-lock"] = {DoubleLock::define_states};
    result["double-unlock"] = {DoubleUnlock::defineStates};
    result["memory-leak"] = {MemoryLeak::defineStates};
    result["null-deref"] = {NullPointer::defineStates};
    result["use-after-free"] = {UseAfterFree::defineStates};
    result["use-before-init"] = {UseBeforeInitialization::defineStates};
    result["ref-count"] = {ReferenceCounter::defineStates};
    result["ref-uncount"] = {UnreferenceCounter::defineStates};
    return result;
  }();
  return registry;
}

std::unique_ptr<fitx::FrameworkPass>
createDetector(const std::set<std::string> &checks) {
  DetectorDefinitions definitions;
  for (const std::string &check : checks) {
    auto detector = detectorRegistry().find(check);
    if (detector == detectorRegistry().end()) {
      return nullptr;
    }
    definitions.insert(definitions.end(), detector->second.begin(),
                       detector->second.end());
  }
  return std::make_unique<SelectedDetector>(std::move(definitions));
}

} // namespace

namespace fitx {

// Run all registered FiTx checkers via the legacy pass manager.
bool runFiTxAnalysis(Module &M, const std::set<std::string> &checks) {
  std::unique_ptr<FrameworkPass> detector = createDetector(checks);
  if (!detector) {
    errs() << "error: invalid FiTx checker selection\n";
    return false;
  }

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Info)) {
    outs() << "FiTx Bug Finder\n";
    outs() << "================\n\n";
    outs() << "Module: " << M.getName() << "\n";
    outs() << "Functions: " << M.size() << "\n";
    outs() << "Checks:";
    for (const std::string &check : checks) {
      outs() << " " << check;
    }
    outs() << "\n";
  }

  // Ensure LoopInfoWrapperPass is initialized (required by IRGenerator).
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeLoopInfoWrapperPassPass(Registry);

  legacy::PassManager PM;

  // 1. Build framework IR for all functions (required by FrameworkPass).
  PM.add(new LoopInfoWrapperPass());
  PM.add(new ir_generator::IRGenerator());

  // 2. Run only the detector selected by the CLI.
  PM.add(detector.release());

  PM.run(M);

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Info))
    outs() << "Analysis complete.\n";
  return true;
}

} // namespace fitx

int runFiTxCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto selectedOr =
      lotus::checker::tooling::resolveChecks(lotus::checker::EngineKind::FiTx);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  fitx::setDebugLogging(lotus::checker::tooling::logAtLeast(
      lotus::checker::tooling::LogLevel::Debug));

  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("fitx", *M, mgr);

  if (!fitx::runFiTxAnalysis(*M, *selectedOr)) {
    return lotus::checker::tooling::EXIT_ERROR;
  }
  stats.emit();

  return lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});
}
