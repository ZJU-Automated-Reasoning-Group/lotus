//===- lotus-check-symex.cpp -- Symbolic Execution Bug Checker ------------===//
//
// Lotus frontend for the SymbolicExecution engine. The tool parses LLVM IR,
// runs the legacy SymbolicExecutionWrapper module pass, and emits findings via
// the shared BugReportMgr reporting pipeline.
//
//===----------------------------------------------------------------------===//

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/ReportOptions.h"
#include "Checker/Framework/Subcommands.h"
#include "Checker/Framework/SuppressionManager.h"
#include "CheckerOptions.h"
#include "CheckerReport.h"
#include "IR/GSA/GSA.h"
#include "IR/GVFG/GuardedValueFlowBuilder.h"
#include "IR/GVFG/LotusAdapter.h"
#include "SymbolicExecution/SymbolicExecutionWrapper.h"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFilename(cl::Positional, cl::desc("<input bitcode file>"),
                  cl::Required,
                  cl::sub(lotus::checker::tooling::symexSubCommand()));

int runSymExCheckerTool(const char *argv0) {
  (void)lotus::checker::tooling::statsEnabled();
  auto selectedOr = lotus::checker::tooling::resolveChecks(
      lotus::checker::EngineKind::SymExec);
  if (!selectedOr) {
    logAllUnhandledErrors(selectedOr.takeError(), errs(), "error: ");
    return lotus::checker::tooling::EXIT_ERROR;
  }
  const auto &selected = *selectedOr;

  unsigned bugTypes = SymbolicExecution::AnalysisState::BUG_TY_UNDEF;
  const std::pair<StringRef, SymbolicExecution::AnalysisState::SymexBugType>
      checkTypes[] = {
          {"buffer-overflow", SymbolicExecution::AnalysisState::BUG_TY_BOF},
          {"div-by-zero", SymbolicExecution::AnalysisState::BUG_TY_DBZ},
          {"int-overflow",
           SymbolicExecution::AnalysisState::BUG_TY_INT_OVERFLOW},
          {"int-underflow",
           SymbolicExecution::AnalysisState::BUG_TY_INT_UNDERFLOW},
          {"null-deref", SymbolicExecution::AnalysisState::BUG_TY_NULL_DEREF},
          {"signed-int-overflow",
           SymbolicExecution::AnalysisState::BUG_TY_SIGNED_INT_OVERFLOW},
          {"signed-int-underflow",
           SymbolicExecution::AnalysisState::BUG_TY_SIGNED_INT_UNDERFLOW},
          {"shift-overflow",
           SymbolicExecution::AnalysisState::BUG_TY_SHIFT_OVERFLOW},
          {"array-oob",
           SymbolicExecution::AnalysisState::BUG_TY_ARRAY_INDEX_OOB},
          {"uninitialized-read",
           SymbolicExecution::AnalysisState::BUG_TY_UNINIT_READ},
          {"use-after-free", SymbolicExecution::AnalysisState::BUG_TY_UAF},
          {"double-free", SymbolicExecution::AnalysisState::BUG_TY_DOUBLE_FREE},
          {"negative-array-index",
           SymbolicExecution::AnalysisState::BUG_TY_NEGATIVE_ARRAY_INDEX},
          {"int-truncation",
           SymbolicExecution::AnalysisState::BUG_TY_INT_TRUNCATION},
      };
  for (const auto &[id, type] : checkTypes) {
    if (selected.count(id.str())) {
      bugTypes |= type;
    }
  }
  SymbolicExecution::AnalysisDriver::setEnabledBugTypes(bugTypes);

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv0, errs());
    return lotus::checker::tooling::EXIT_ERROR;
  }
  BugReportMgr &mgr = BugReportMgr::get_instance();
  lotus::checker::tooling::AnalysisStatsRecorder stats("symex", *M, mgr);

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTransformUtils(Registry);
  initializeScalarOpts(Registry);
  initializeIPO(Registry);
  initializeInstCombine(Registry);
  initializeTarget(Registry);

  legacy::PassManager PM;
  PM.add(new gsa::ControlDependenceAnalysisPass());
  PM.add(new gsa::GateAnalysisPass());
  PM.add(new LotusAA());
  PM.add(new lotus::gvfg::GuardedValueFlowGraphBuilderPass());
  PM.add(new lotus::gvfg::LotusGuardedValueFlowAdapterPass());
  PM.add(new SymbolicExecutionWrapper());
  PM.run(*M);
  stats.emit();

  const int reportStatus = lotus::checker::tooling::emitCheckerReports(
      mgr, {lotus::checker::tooling::Verbose});

  if (lotus::checker::tooling::logAtLeast(
          lotus::checker::tooling::LogLevel::Info))
    outs() << "\n=== Analysis Complete ===\n";
  return reportStatus;
}
