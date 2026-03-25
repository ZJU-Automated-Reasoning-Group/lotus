/*
 * LotusAA Driver
 * This file implements the main driver for running LotusAA pointer analysis
 * on LLVM bitcode files. It parses command-line options, loads the input
 * module, runs the LotusAA analysis pass, and outputs the results.
 *
 */

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/LotusAA/Engine/InterProceduralPass.h"

#include <memory>

#include <llvm/IR/Dominators.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/InitializePasses.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Signals.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string> OutputFilename("o",
                                           cl::desc("Override output filename"),
                                           cl::value_desc("filename"));

static cl::opt<bool>
    OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                   cl::init(false));

static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"),
                                    cl::init(false));

static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));

// LotusAA-specific options are defined in InterProceduralPass.cpp:
// -lotus-cg: Use LotusAA to build call graph
// -lotus-restrict-cg-iter: Maximum iterations for call graph construction
// -lotus-enable-global-heuristic: Enable heuristic for global pointer handling
// -lotus-print-pts: Print LotusAA points-to results
// -lotus-print-cg: Print LotusAA call graph results
// -lotus-restrict-inline-depth: Maximum inlining depth for inter-procedural
// analysis -lotus-restrict-cg-size: Maximum indirect call targets to process

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // Initialize passes
  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);

  cl::ParseCommandLineOptions(argc, argv, "LotusAA Pointer Analysis Tool\n");

  LLVMContext Context;
  SMDiagnostic Err;

  // Load the input module
  auto M = loadIRModule(InputFilename, Context, Err, argv[0]);
  if (!M) {
    return 1;
  }

  // Verify the module
  if (verifyModule(*M, &errs())) {
    errs() << "Error: Module verification failed\n";
    return 1;
  }

  // Create output file if specified
  std::unique_ptr<ToolOutputFile> Out;
  if (!OutputFilename.empty()) {
    std::error_code EC;
    Out =
        std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }
  }

  // Set up the output stream
  raw_ostream &OS = Out ? Out->os() : outs();

  if (Verbose) {
    errs() << "Starting LotusAA Pointer Analysis...\n";
    errs() << "Input file: " << InputFilename << "\n";
    errs() << "Module: " << M->getName() << "\n";
    errs() << "Functions: " << M->getFunctionList().size() << "\n";
    errs() << "Global variables: " << M->getGlobalList().size() << "\n\n";
  }

  // Create the pass manager and add the LotusAA pass
  legacy::PassManager PM;
  PM.add(new LotusAA());

  // Run the analysis
  if (Verbose) {
    errs() << "Running LotusAA analysis...\n";
  }

  PM.run(*M);

  if (Verbose) {
    errs() << "\nLotusAA analysis complete.\n";
  }

  // Note: Results are printed by the pass itself based on command-line flags:
  // Use -lotus-print-pts to print points-to information
  // Use -lotus-print-cg to print call graph information

  if (!OnlyStatistics) {
    OS << "LotusAA analysis completed successfully.\n";
    OS << "Use -lotus-print-pts to see points-to results\n";
    OS << "Use -lotus-print-cg to see call graph results\n";
  }

  // Write output file if specified
  if (Out) {
    Out->keep();
  }

  return 0;
}
