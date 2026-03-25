#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/AserPTA/PreProcessing/Passes/CanonicalizeGEPPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/LoweringMemCpyPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/RemoveASMInstPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/RemoveExceptionHandlerPass.h"
#include "Alias/AserPTA/PreProcessing/Passes/StandardHeapAPIRewritePass.h"
#include "Alias/DFPA/DFPAPass.h"

#include <fstream>
#include <memory>
#include <string>

using namespace llvm;
using namespace dfpa;
using namespace lotus::alias::tools;

namespace {

cl::list<std::string> InputFilenames(cl::Positional, cl::OneOrMore,
                                     cl::desc("<input bitcode files>"));
cl::opt<unsigned>
    IndirectCtxK("indirect-ctx-k",
                 cl::desc("Selective context depth on indirect edges"),
                 cl::init(1));
cl::opt<bool> RefineAmbiguousOnly(
    "refine-ambiguous-only",
    cl::desc("Refine only ambiguous or unknown indirect calls"), cl::init(true));
cl::opt<unsigned> MaxOffsetDepth("max-offset-depth",
                                 cl::desc("Maximum offset path depth"),
                                 cl::init(8));
cl::opt<unsigned long long> MaxDemandStates(
    "max-demand-states", cl::desc("Demand refinement state budget"),
    cl::init(50000));
cl::opt<bool> EnableSignatureFilter(
    "enable-signature-filter",
    cl::desc("Intersect candidates with normalized signature matches"),
    cl::init(true));
cl::opt<std::string> OutputFilePath(
    "output-file",
    cl::desc("Output file path, or 'cout' for standard output"), cl::init(""));

void printStats(raw_ostream &OS, const DFPAResult &Result) {
  const DFPAStats &Stats = Result.getStats();
  OS << "############## DFPA Result Statistics ##############\n";
  OS << "# Number of indirect calls:\t\t" << Stats.num_indirect_calls << "\n";
  OS << "# Number of refined calls:\t\t" << Stats.num_refined_calls << "\n";
  OS << "# Number of precise calls:\t\t" << Stats.num_precise_calls << "\n";
  OS << "# Number of budget fallbacks:\t\t" << Stats.num_budget_fallbacks
     << "\n";
  OS << "# Number of unknown-slot degradations:\t"
     << Stats.num_unknown_slot_degradations << "\n";
  OS << "# Coarse avg. targets per call:\t\t" << Stats.coarseAvgTargets()
     << "\n";
  OS << "# Refined avg. targets per call:\t\t" << Stats.refinedAvgTargets()
     << "\n";
}

std::string locationString(const CallBase *CB) {
  if (!CB || !CB->getDebugLoc())
    return "<unknown>:0:0";
  const DebugLoc &Loc = CB->getDebugLoc();
  return Loc->getFilename().str() + ":" + std::to_string(Loc.getLine()) + ":" +
         std::to_string(Loc.getCol());
}

void dumpTargets(raw_ostream &OS, const DFPAResult &Result) {
  for (const auto &Entry : Result.getAllTargets()) {
    OS << locationString(Entry.first) << "|";
    bool First = true;
    for (Function *Target : Entry.second.targets) {
      if (!First)
        OS << ",";
      OS << Target->getName();
      First = false;
    }
    OS << "\n";
  }
}

} // namespace

int main(int argc, char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);
  llvm_shutdown_obj Shutdown;
  InitLLVM Init(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "DFPA research prototype\n");

  for (const std::string &InputFilename : InputFilenames) {
    LLVMContext Context;
    SMDiagnostic Err;
    auto M = loadIRModule(InputFilename, Context, Err, argv[0]);
    if (!M) {
      errs() << argv[0] << ": error loading file '" << InputFilename << "'\n";
      continue;
    }

    legacy::PassManager PM;
    PM.add(new aser::CanonicalizeGEPPass());
    PM.add(new aser::LoweringMemCpyPass());
    PM.add(new aser::RemoveExceptionHandlerPass());
    PM.add(new aser::RemoveASMInstPass());
    PM.add(new StandardHeapAPIRewritePass());

    DFPAConfig Config;
    Config.indirect_ctx_k = IndirectCtxK;
    Config.refine_ambiguous_only = RefineAmbiguousOnly;
    Config.max_demand_states = MaxDemandStates;
    Config.max_offset_depth = MaxOffsetDepth;
    Config.enable_signature_filter = EnableSignatureFilter;

    auto *Pass = new DFPAPass(Config);
    PM.add(Pass);
    PM.run(*M);

    const DFPAResult &Result = Pass->getResult();
    printStats(outs(), Result);
    if (OutputFilePath.empty())
      continue;

    if (OutputFilePath == "cout") {
      dumpTargets(outs(), Result);
      continue;
    }

    std::error_code EC;
    raw_fd_ostream OS(OutputFilePath, EC, sys::fs::OF_None);
    if (EC) {
      errs() << "failed to open output file '" << OutputFilePath
             << "': " << EC.message() << "\n";
      return 1;
    }
    dumpTargets(OS, Result);
  }

  return 0;
}
