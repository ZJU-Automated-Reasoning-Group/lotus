//===- lotus-check-saber.cpp -- Source-sink bug checker (Saber) ------------//
//
// Lotus tool for memory leak, double-free, and file descriptor checks.
// Mirrors SVF's saber tool; uses Saber engine on Lotus SVFG.
//
//===----------------------------------------------------------------------===//

#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "Checker/Saber/DoubleFreeChecker.h"
#include "Checker/Saber/FileChecker.h"
#include "Checker/Saber/LeakChecker.h"
#include "Checker/Saber/SaberOptions.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"
#include "Utils/LLVM/RecursiveTimer.h"

#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::Required,
                  cl::sub(lotus::checker::tooling::saberSubCommand()));

int runSaberCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto selectedOr =
      lotus::checker::tooling::resolveChecks(lotus::checker::EngineKind::Saber);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const auto &selected = *selectedOr;
  lotus::analysis::SaberVerbose = lotus::checker::tooling::logAtLeast(
      lotus::checker::tooling::LogLevel::Debug);
  RecursiveTimer::setEnabled(false);

  // Force linkage of SaberOptions symbols from static library
  // Reference the extern variables to ensure they're linked
  (void)&lotus::analysis::SaberFullSVFG;
  (void)&lotus::analysis::SaberCxtLimit;
  (void)&lotus::analysis::SaberMaxStepInWrapper;
  (void)&lotus::analysis::SaberDumpSlice;
  (void)&lotus::analysis::SaberValidateTests;
  (void)&lotus::analysis::SaberCollectExtRetGlobals;

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("saber", *M, mgr);

  const bool runLeak = selected.count("memory-leak");
  const bool runDoubleFree = selected.count("double-free");
  const bool runFile = selected.count("file-leak");

  // Count how many checkers will run
  int checkerCount = 0;
  if (runLeak)
    checkerCount++;
  if (runDoubleFree)
    checkerCount++;
  if (runFile)
    checkerCount++;

  // If running multiple checkers, build SVFG/ICFG once and share them
  std::unique_ptr<lotus::analysis::SVFG> shared_svfg;
  std::unique_ptr<::ICFG> shared_icfg;
  lotus::analysis::SrcSnkDDA::RemovedSUVFEdges shared_removed_su_vfg_edges;

  if (checkerCount > 1) {
    // Build SVFG/ICFG once using a temporary checker
    outs() << "\n=== Building SVFG (shared across checkers) ===\n";
    lotus::analysis::LeakChecker builderChecker;
    builderChecker.setModule(M.get());
    builderChecker.initialize();
    builderChecker.exportRemovedSUVFEdges(shared_removed_su_vfg_edges);

    // Extract SVFG/ICFG to share (ownership moves to shared_svfg/shared_icfg)
    auto extracted = builderChecker.extractSVFGAndICFG();
    shared_svfg = std::move(extracted.first);
    shared_icfg = std::move(extracted.second);
  }

  // Run each checker
  if (runLeak) {
    if (checkerCount > 1) {
      outs() << "\n=== Running Memory Leak Checker ===\n";
    } else {
      outs() << "Running Memory Leak checker...\n";
    }
    lotus::analysis::LeakChecker leakChecker;
    if (checkerCount > 1 && shared_svfg && shared_icfg) {
      // Move shared graphs into this checker.
      leakChecker.setSharedSVFGAndICFG(std::move(shared_svfg),
                                       std::move(shared_icfg));
      leakChecker.importRemovedSUVFEdges(shared_removed_su_vfg_edges);
    }
    leakChecker.setModule(M.get());
    leakChecker.runOnModule(*M);
    if (checkerCount > 1) {
      // Hand graphs to the next checker only after this one has finished.
      leakChecker.exportRemovedSUVFEdges(shared_removed_su_vfg_edges);
      auto extracted = leakChecker.extractSVFGAndICFG();
      shared_svfg = std::move(extracted.first);
      shared_icfg = std::move(extracted.second);
    }
  }

  if (runDoubleFree) {
    if (checkerCount > 1) {
      outs() << "\n=== Running Double Free Checker ===\n";
    } else {
      outs() << "Running Double Free checker...\n";
    }
    lotus::analysis::DoubleFreeChecker dfChecker;
    if (checkerCount > 1 && shared_svfg && shared_icfg) {
      dfChecker.setSharedSVFGAndICFG(std::move(shared_svfg),
                                     std::move(shared_icfg));
      dfChecker.importRemovedSUVFEdges(shared_removed_su_vfg_edges);
    }
    // Note: Double-free checker uses free() calls as both sources and sinks,
    // so it doesn't share source/sink state with leak checker.
    dfChecker.setModule(M.get());
    dfChecker.runOnModule(*M);
    if (checkerCount > 1) {
      dfChecker.exportRemovedSUVFEdges(shared_removed_su_vfg_edges);
      auto extracted = dfChecker.extractSVFGAndICFG();
      shared_svfg = std::move(extracted.first);
      shared_icfg = std::move(extracted.second);
    }
  }

  if (runFile) {
    if (checkerCount > 1) {
      outs() << "\n=== Running File Descriptor Checker ===\n";
    } else {
      outs() << "Running File Descriptor checker...\n";
    }
    lotus::analysis::FileChecker fileChecker;
    if (checkerCount > 1 && shared_svfg && shared_icfg) {
      fileChecker.setSharedSVFGAndICFG(std::move(shared_svfg),
                                       std::move(shared_icfg));
      fileChecker.importRemovedSUVFEdges(shared_removed_su_vfg_edges);
    }
    fileChecker.setModule(M.get());
    fileChecker.runOnModule(*M);
  }

  // Print bug report summary
  stats.emit();

  const int reportStatus = lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});

  if (checkerCount > 1) {
    outs() << "\n=== Analysis Complete ===\n";
  }

  return reportStatus;
}
