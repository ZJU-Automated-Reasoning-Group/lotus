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
 * Usage: lotus-fitx [options] <input bitcode>
 */

#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Framework_IR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

static cl::opt<std::string>
    InputFile(cl::Positional, cl::desc("<input bitcode>"), cl::Required);
static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<bool> MeasureAnalysisTime("fitx-measure",
                                         cl::desc("Measure analysis time"),
                                         cl::init(false));
static cl::opt<std::string>
    DetectorType("detector",
                 cl::desc("Detector type: all, df, dl, dul, leak, nullptr, "
                          "uaf, ubi, ref_count, ref_uncount"),
                 cl::init("all"));

namespace fitx {

// Run all registered FiTx checkers via the legacy pass manager.
void runFiTxAnalysis(Module &M) {
  errs() << "FiTx Bug Finder\n";
  errs() << "================\n\n";
  errs() << "Module: " << M.getName() << "\n";
  errs() << "Functions: " << M.size() << "\n";
  errs() << "Checkers: " << FrameworkPass::passes.size() << "\n\n";

  auto start = std::chrono::system_clock::now();

  // Ensure LoopInfoWrapperPass is initialized (required by IRGenerator).
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeLoopInfoWrapperPassPass(Registry);

  legacy::PassManager PM;

  // 1. Build framework IR for all functions (required by FrameworkPass).
  PM.add(new LoopInfoWrapperPass());
  PM.add(new ir_generator::IRGenerator());

  // 2. Run all registered FiTx checkers (e.g. AllDetector runs df, dl, dul,
  //    leak, ref_count, uaf).
  for (fitx::FrameworkPass *P : fitx::FrameworkPass::passes) {
    PM.add(P);
  }

  PM.run(M);

  auto end = std::chrono::system_clock::now();
  auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  errs() << "\nAnalysis complete!\n";
  errs() << "Time: " << duration.count() << " ms\n";
}

} // namespace fitx

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "FiTx Bug Finder\n");

  SMDiagnostic Err;
  LLVMContext Context;
  std::unique_ptr<Module> M = parseIRFile(InputFile, Err, Context);

  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  fitx::runFiTxAnalysis(*M);

  return 0;
}
