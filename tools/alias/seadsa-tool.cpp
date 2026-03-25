///
// seadsda-tool -- Advanced memory graph and call graph analysis tool for
// sea-dsa
///

// #include "llvm/Analysis/CallPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
// #include "llvm/Support/ManagedStatic.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/seadsa/CompleteCallGraph.hh"
#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/DsaLibFuncInfo.hh"
#include "Alias/seadsa/InitializePasses.hh"
#include "Alias/seadsa/SeaDsaAliasAnalysis.hh"
#include "Alias/seadsa/support/RemovePtrToInt.hh"
// #include "Alias/seadsa/support/Debug.h"

static llvm::cl::opt<std::string>
    InputFilename(llvm::cl::Positional,
                  llvm::cl::desc("<input LLVM bitcode file>"),
                  llvm::cl::Required, llvm::cl::value_desc("filename"));

static llvm::cl::opt<std::string> OutputDir("outdir",
                                            llvm::cl::desc("Output directory"),
                                            llvm::cl::init(""),
                                            llvm::cl::value_desc("DIR"));

static llvm::cl::opt<std::string>
    AsmOutputFilename("output", llvm::cl::desc("Output analyzed bitcode"),
                      llvm::cl::init(""), llvm::cl::value_desc("filename"));

static llvm::cl::opt<bool> MemDot(
    "sea-dsa-dot",
    llvm::cl::desc("Print SeaDsa memory graph of each function to dot format"),
    llvm::cl::init(false));

static llvm::cl::opt<bool> CallGraphDot(
    "sea-dsa-callgraph-dot",
    llvm::cl::desc("Print SeaDsa complete call graph to dot format"),
    llvm::cl::init(false));

static llvm::cl::opt<bool>
    AAEval("sea-dsa-aa-eval",
           llvm::cl::desc("Evaluate alias analysis precision using seadsa"),
           llvm::cl::init(false));

// Register passes manually since INITIALIZE_PASS macros are commented out
static llvm::RegisterPass<seadsa::DsaAnalysis>
    X("seadsa-dsa", "SeaHorn Dsa analysis: entry point for all clients");
static llvm::RegisterPass<seadsa::RemovePtrToInt>
    Y("seadsa-remove-ptrtoint",
      "Convert ptrtoint/inttoptr pairs to bitcasts when possible");
static llvm::RegisterPass<seadsa::AllocWrapInfo>
    Z("seadsa-alloc-wrap", "Identifies allocation wrappers");
static llvm::RegisterPass<seadsa::DsaLibFuncInfo>
    W("seadsa-lib-functions-info",
      "Identifies library functions for special handling");

/// Changes the \p path to be inside specified \p outdir
///
/// Creates output directory if necessary.
/// Returns input \p path if directory cannot be changed
static std::string withDir(const std::string &outdir, const std::string &path) {
  if (!outdir.empty()) {
    if (!llvm::sys::fs::create_directories(outdir)) {
      auto fname = llvm::sys::path::filename(path);
      return outdir + "/" + fname.str();
    }
  }
  return path;
}

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(
      argc, argv, "Sea-DSA Advanced Memory Graph Analysis Tool");
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::EnableDebugBuffering = true;

  llvm::LLVMContext context;
  llvm::SMDiagnostic err;
  std::unique_ptr<llvm::ToolOutputFile> asmOutput;

  // Load the input module
  auto module =
      lotus::alias::tools::loadIRModule(InputFilename, context, err, argv[0]);
  if (!module) {
    if (llvm::errs().has_colors())
      llvm::errs().changeColor(llvm::raw_ostream::RED);
    llvm::errs() << "error: "
                 << "Bitcode was not properly read; " << err.getMessage()
                 << "\n";
    if (llvm::errs().has_colors())
      llvm::errs().resetColor();
    return 3;
  }

  // Set up output file if requested
  if (!AsmOutputFilename.empty()) {
    std::error_code error_code;
    asmOutput = std::make_unique<llvm::ToolOutputFile>(
        withDir(OutputDir, AsmOutputFilename), error_code,
        llvm::sys::fs::OF_Text);
    if (error_code) {
      if (llvm::errs().has_colors())
        llvm::errs().changeColor(llvm::raw_ostream::RED);
      llvm::errs() << "error: Could not open " << AsmOutputFilename << ": "
                   << error_code.message() << "\n";
      if (llvm::errs().has_colors())
        llvm::errs().resetColor();
      return 3;
    }
  }

  // Initialize and run passes
  llvm::legacy::PassManager PM;

  // Initialize passes
  llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeCore(Registry);
  seadsa::initializeAnalysisPasses(Registry);

  // Add sea-dsa passes
  PM.add(new seadsa::RemovePtrToInt());
  PM.add(new seadsa::AllocWrapInfo());
  PM.add(new seadsa::DsaLibFuncInfo());
  PM.add(new seadsa::DsaAnalysis());

  // Add selected analysis outputs
  if (MemDot) {
    PM.add(seadsa::createDsaPrinterPass());
  }

  if (CallGraphDot) {
    // This pass is not properly initialized, comment out for now
    // PM.add(seadsa::createDsaCallGraphPrinterPass());
    llvm::errs()
        << "Warning: Call graph printing is not available in this version\n";
  }

  if (AAEval) {
    // Comment out the unavailable AAEvalPass
    // PM.add(llvm::createAAEvalPass());
    llvm::errs()
        << "Warning: AA evaluation is not available in this LLVM version\n";
  }

  if (!MemDot && !CallGraphDot && !AAEval) {
    llvm::errs() << "No option selected: choose at least one option between "
                 << "{sea-dsa-dot, sea-dsa-callgraph-dot, sea-dsa-aa-eval}\n";
  }

  // Add output pass if requested
  if (!AsmOutputFilename.empty()) {
    // Use a different way to create a print module pass
    llvm::errs()
        << "Warning: Cannot add module printing pass in this LLVM version\n";
  }

  // Run the passes
  PM.run(*module.get());

  // Keep output file if needed
  if (!AsmOutputFilename.empty())
    asmOutput->keep();

  return 0;
}