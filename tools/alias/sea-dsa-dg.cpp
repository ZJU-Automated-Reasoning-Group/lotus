///
// sea-dsa-dg -- Print heap graphs computed by sea-dsa
///

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CommandLine.h"
// #include "llvm/Support/FileSystem.h"
// #include "llvm/Support/Path.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/DsaPrinter.hh"
#include "Alias/seadsa/Global.hh"
#include "Alias/seadsa/InitializePasses.hh"

// Add these includes for proper registration
#include "Alias/seadsa/AllocWrapInfo.hh"
#include "Alias/seadsa/DsaLibFuncInfo.hh"
#include "Alias/seadsa/support/RemovePtrToInt.hh"

static llvm::cl::opt<std::string>
    InputFilename(llvm::cl::Positional,
                  llvm::cl::desc("<input LLVM bitcode file>"),
                  llvm::cl::Required, llvm::cl::value_desc("filename"));

static llvm::cl::opt<bool>
    MemDot("sea-dsa-dot",
           llvm::cl::desc("Print memory graph of each function to dot format"),
           llvm::cl::init(false));

static llvm::cl::opt<bool>
    UseOpaquePointers("use-opaque-ptrs", llvm::cl::desc("Use opaque pointers"),
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

int main(int argc, char **argv) {
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "Sea-DSA Memory Graph Analysis");
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::EnableDebugBuffering = true;

  // Initialize passes
  llvm::PassRegistry &Registry = *llvm::PassRegistry::getPassRegistry();
  llvm::initializeCore(Registry);
  seadsa::initializeAnalysisPasses(Registry);

  // Load the input module
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Err;
  auto M =
      lotus::alias::tools::loadIRModule(InputFilename, Context, Err, argv[0]);
  if (!M) {
    return 1;
  }

  llvm::legacy::PassManager PM;

  // Add sea-dsa passes
  PM.add(new seadsa::RemovePtrToInt());
  PM.add(new seadsa::AllocWrapInfo());
  PM.add(new seadsa::DsaLibFuncInfo());
  PM.add(new seadsa::DsaAnalysis());

  if (MemDot) {
    PM.add(seadsa::createDsaPrinterPass());
  }

  // Run the passes
  PM.run(*M.get());

  return 0;
}