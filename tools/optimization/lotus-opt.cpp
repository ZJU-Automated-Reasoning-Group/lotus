/*
 * lotus-opt
 * A command-line driver for inter-procedural optimizations in lib/Optimization.
 */

#include "Alias/seadsa/AllocSiteInfo.hh"
#include "Alias/seadsa/AllocWrapInfo.hh"
#include "Alias/seadsa/DsaAnalysis.hh"
#include "Alias/seadsa/DsaLibFuncInfo.hh"
#include "Alias/seadsa/InitializePasses.hh"
#include "Alias/seadsa/ShadowMem.hh"
#include "Alias/seadsa/support/RemovePtrToInt.hh"
#include "Verification/Analysis/Analysis.h"
#include "Verification/Transform/Instrumentation.h"

#include <memory>
#include <string>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/InitializePasses.h>
#include <llvm/Pass.h>
#include <llvm/PassRegistry.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO.h>

using namespace llvm;
using namespace seadsa;

namespace {

static cl::OptionCategory OptCat("Lotus Optimization Tool");

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required, cl::cat(OptCat));

static cl::opt<std::string>
    OutputFilename("o", cl::desc("Override output filename (default: -)"),
                   cl::value_desc("filename"), cl::init("-"), cl::cat(OptCat));

static cl::opt<bool>
    OutputAssembly("S", cl::desc("Write LLVM assembly instead of bitcode"),
                   cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableAllIP("ip-all", cl::desc("Enable all inter-procedural optimizations"),
                cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableAInline("ainline",
                                   cl::desc("Run aggressive inliner"),
                                   cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPDSE("ipdse",
                cl::desc("Run inter-procedural dead store elimination"),
                cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPRLE("ip-rle",
                cl::desc("Run inter-procedural redundant load elimination"),
                cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPSink("ip-sink", cl::desc("Run inter-procedural store sinking"),
                 cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableIPForward("ip-forward",
                    cl::desc("Run inter-procedural store-to-load forwarding"),
                    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableInitUninit(
    "init-uninit",
    cl::desc("Initialize stack allocas with nondeterministic values"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableMakeNondet(
    "make-nondet",
    cl::desc("Replace selected input functions with nondeterministic values"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnablePrepOverflows(
    "prep-overflows",
    cl::desc("Instrument signed arithmetic with explicit overflow checks"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableBreakCritLoops("break-crit-loops",
                         cl::desc("Break critical loops for better slicing"),
                         cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableDeleteUndefined(
    "delete-undefined",
    cl::desc("Delete calls to undefined functions, replace with nondet"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableRemoveConstantExprs(
    "remove-constant-exprs",
    cl::desc("Transform constant expressions to instructions"), cl::init(false),
    cl::cat(OptCat));

static cl::opt<bool> EnableInternalizeGlobals(
    "internalize-globals",
    cl::desc("Internalize and make non-deterministic external globals"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableRemoveErrorCalls("remove-error-calls",
                           cl::desc("Remove calls to __VERIFIER_error"),
                           cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableInstrumentAlloc(
    "instrument-alloc",
    cl::desc("Replace calls to malloc and calloc with verifier functions"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableInstrumentAllocNF(
    "instrument-alloc-nf",
    cl::desc(
        "Replace calls to malloc/calloc with verifier functions (never fails)"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableRemoveInfiniteLoops(
    "remove-infinite-loops",
    cl::desc("Delete patterns like LABEL: goto LABEL and replace with exit(0)"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableBreakInfiniteLoops(
    "break-infinite-loops",
    cl::desc("Transform loops that have no exit to loops that have an exit"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableFlattenLoops("flatten-loops",
                       cl::desc("Flatten nested loops into non-nested loops"),
                       cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableInstrumentNontermination(
    "instrument-nontermination",
    cl::desc("Insert trivial checks for state space cycles"), cl::init(false),
    cl::cat(OptCat));

static cl::opt<bool> EnableRemoveReadOnlyAttr(
    "remove-readonly-attr",
    cl::desc("Remove read-only attribute from selected functions"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableRenameVerifierFuns(
    "rename-verifier-funs",
    cl::desc(
        "Replace calls to verifier functions with calls to named versions"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableReplaceLifetimeMarkers(
    "replace-lifetime-markers",
    cl::desc("Replace lifetime markers with calls to __VERIFIER_scope_*"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableMarkVolatile("mark-volatile",
                       cl::desc("Make marked instructions as volatile"),
                       cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableFindExits(
    "find-exits",
    cl::desc(
        "Put calls to __VERIFIER_silent_exit into bitcode before any exit"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableDummyMarker(
    "dummy-marker",
    cl::desc(
        "Put calls to dummy functions into bitcode to prevent code removal"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableUnrolling("lotus-loop-unroll",
                    cl::desc("Unroll loops a specified number of times"),
                    cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableExplicitConsdes(
    "explicit-consdes",
    cl::desc("Insert explicit calls of module constructors and destructors"),
    cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableDeleteCalls("delete-calls",
                      cl::desc("Delete direct calls to specified functions"),
                      cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableReplaceVerifierAtomic(
    "replace-verifier-atomic",
    cl::desc("Replace verifier atomic function names"), cl::init(false),
    cl::cat(OptCat));

static cl::opt<bool> EnableClassifyInstructions(
    "classify-instructions",
    cl::desc("Print statistics about instruction types"), cl::init(false),
    cl::cat(OptCat));

static cl::opt<bool>
    EnableClassifyLoops("classify-loops",
                        cl::desc("Detect what loops are in the program"),
                        cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableCountInstr("count-instr",
                                      cl::desc("Print statistics from module"),
                                      cl::init(false), cl::cat(OptCat));

static cl::opt<bool>
    EnableGetTestTargets("get-test-targets",
                         cl::desc("Find targets for tests generation"),
                         cl::init(false), cl::cat(OptCat));

static cl::opt<bool> EnableCheckModule(
    "check-module",
    cl::desc("Check whether the module contains given features"),
    cl::init(false), cl::cat(OptCat));

static bool addPassByName(legacy::PassManager &PM, StringRef PassName) {
  const PassRegistry &Registry = *PassRegistry::getPassRegistry();
  const PassInfo *PI = Registry.getPassInfo(PassName);
  if (!PI) {
    errs() << "error: unknown pass '" << PassName << "'\n";
    return false;
  }
  PM.add(PI->createPass());
  return true;
}

} // namespace

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeTransformUtils(Registry);
  initializeIPO(Registry);
  initializeCallGraphWrapperPassPass(Registry);
  initializeGlobalsAAWrapperPassPass(Registry);
  initializeTargetLibraryInfoWrapperPassPass(Registry);
  initializeDominatorTreeWrapperPassPass(Registry);
  initializeAssumptionCacheTrackerPass(Registry);

  // Initialize SeaDsa passes
  initializeAnalysisPasses(Registry);
  initializeRemovePtrToIntPass(Registry);
  initializeAllocWrapInfoPass(Registry);
  initializeDsaLibFuncInfoPass(Registry);
  initializeAllocSiteInfoPass(Registry);
  initializeDsaAnalysisPass(Registry);
  initializeShadowMemPassPass(Registry);

  cl::ParseCommandLineOptions(
      argc, argv, "Lotus optimization tool for inter-procedural passes\n");

  if (EnableAllIP) {
    EnableAInline = true;
    EnableIPDSE = true;
    EnableIPRLE = true;
    EnableIPSink = true;
    EnableIPForward = true;
    EnableInitUninit = true;
    EnableMakeNondet = true;
    EnablePrepOverflows = true;
    EnableBreakCritLoops = true;
    EnableDeleteUndefined = true;
    EnableRemoveConstantExprs = true;
    EnableInternalizeGlobals = true;
    EnableRemoveErrorCalls = true;
    EnableInstrumentAlloc = true;
    EnableRemoveInfiniteLoops = true;
    EnableBreakInfiniteLoops = true;
    EnableFlattenLoops = true;
    EnableInstrumentNontermination = true;
    EnableRemoveReadOnlyAttr = true;
    EnableRenameVerifierFuns = true;
    EnableReplaceLifetimeMarkers = true;
    EnableMarkVolatile = true;
    EnableUnrolling = true;
    EnableExplicitConsdes = true;
    EnableDeleteCalls = true;
    EnableReplaceVerifierAtomic = true;
  }

  if (!EnableAInline && !EnableIPDSE && !EnableIPRLE && !EnableIPSink &&
      !EnableIPForward && !EnableInitUninit && !EnableMakeNondet &&
      !EnablePrepOverflows && !EnableBreakCritLoops && !EnableDeleteUndefined &&
      !EnableRemoveConstantExprs && !EnableInternalizeGlobals &&
      !EnableRemoveErrorCalls && !EnableInstrumentAlloc &&
      !EnableInstrumentAllocNF && !EnableRemoveInfiniteLoops &&
      !EnableBreakInfiniteLoops && !EnableFlattenLoops &&
      !EnableInstrumentNontermination && !EnableRemoveReadOnlyAttr &&
      !EnableRenameVerifierFuns && !EnableReplaceLifetimeMarkers &&
      !EnableMarkVolatile && !EnableFindExits && !EnableDummyMarker &&
      !EnableClassifyInstructions && !EnableClassifyLoops &&
      !EnableCountInstr && !EnableGetTestTargets && !EnableCheckModule &&
      !EnableUnrolling && !EnableExplicitConsdes && !EnableDeleteCalls &&
      !EnableReplaceVerifierAtomic) {
    errs()
        << "error: no optimization selected; use -ip-all or specific flags\n";
    return 1;
  }

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  if (verifyModule(*M, &errs())) {
    errs() << "error: module verification failed\n";
    return 1;
  }

  std::unique_ptr<ToolOutputFile> Out;
  if (!OutputFilename.empty() && OutputFilename != "-") {
    std::error_code EC;
    Out =
        std::make_unique<ToolOutputFile>(OutputFilename, EC, sys::fs::OF_None);
    if (EC) {
      errs() << EC.message() << '\n';
      return 1;
    }
  }

  legacy::PassManager PM;
  bool Ok = true;

  // Check if any IP optimization that requires MemorySSA is enabled
  bool needsMemorySSA =
      EnableIPDSE || EnableIPRLE || EnableIPSink || EnableIPForward;

  // Run aggressive inliner before ShadowMem to avoid breaking
  // shadow.mem/store adjacency assumptions.
  if (EnableAInline)
    Ok &= addPassByName(PM, "ainline");

  // Add prerequisite passes for MemorySSA-based optimizations
  if (needsMemorySSA) {
    // SeaDsa prerequisite passes - must be added in order
    // These passes set up the analysis infrastructure needed by ShadowMem
    PM.add(new RemovePtrToInt());
    PM.add(new AllocWrapInfo());
    PM.add(new DsaLibFuncInfo());
    PM.add(new AllocSiteInfo());
    PM.add(new DsaAnalysis());

    // ShadowMem pass to instrument code with MemorySSA
    // This pass requires all the above passes to have run first
    PM.add(createShadowMemPass());
  }

  // Run MemorySSA-based IP optimizations
  if (EnableIPDSE)
    Ok &= addPassByName(PM, "ipdse");
  if (EnableIPRLE)
    Ok &= addPassByName(PM, "ip-rle");
  if (EnableIPSink)
    Ok &= addPassByName(PM, "ip-sink");
  if (EnableIPForward)
    Ok &= addPassByName(PM, "ip-forward");
  if (EnableInitUninit)
    PM.add(lotus::verification::transform::createInitializeUninitializedPass());
  if (EnableMakeNondet)
    PM.add(lotus::verification::transform::createMakeNondetPass());
  if (EnablePrepOverflows)
    PM.add(lotus::verification::transform::createPrepareOverflowsPass());
  if (EnableBreakCritLoops)
    PM.add(lotus::verification::transform::createBreakCritLoopsPass());
  if (EnableDeleteUndefined)
    PM.add(lotus::verification::transform::createDeleteUndefinedPass());
  if (EnableRemoveConstantExprs)
    PM.add(lotus::verification::transform::createRemoveConstantExprsPass());
  if (EnableInternalizeGlobals)
    PM.add(lotus::verification::transform::createInternalizeGlobalsPass());
  if (EnableRemoveErrorCalls)
    PM.add(lotus::verification::transform::createRemoveErrorCallsPass());
  if (EnableInstrumentAlloc)
    PM.add(lotus::verification::transform::createInstrumentAllocPass());
  if (EnableInstrumentAllocNF)
    PM.add(
        lotus::verification::transform::createInstrumentAllocNeverFailsPass());
  if (EnableRemoveInfiniteLoops)
    PM.add(lotus::verification::transform::createRemoveInfiniteLoopsPass());
  if (EnableBreakInfiniteLoops)
    PM.add(lotus::verification::transform::createBreakInfiniteLoopsPass());
  if (EnableFlattenLoops)
    PM.add(lotus::verification::transform::createFlattenLoopsPass());
  if (EnableInstrumentNontermination)
    PM.add(
        lotus::verification::transform::createInstrumentNonterminationPass());
  if (EnableRemoveReadOnlyAttr)
    PM.add(lotus::verification::transform::createRemoveReadOnlyAttrPass());
  if (EnableRenameVerifierFuns)
    PM.add(lotus::verification::transform::createRenameVerifierFunsPass());
  if (EnableReplaceLifetimeMarkers)
    PM.add(lotus::verification::transform::createReplaceLifetimeMarkersPass());
  if (EnableMarkVolatile)
    PM.add(lotus::verification::transform::createMarkVolatilePass());
  if (EnableFindExits)
    PM.add(lotus::verification::transform::createFindExitsPass());
  if (EnableDummyMarker)
    PM.add(lotus::verification::transform::createDummyMarkerPass());
  if (EnableClassifyInstructions)
    PM.add(lotus::verification::analysis::createClassifyInstructionsPass());
  if (EnableClassifyLoops)
    PM.add(lotus::verification::analysis::createClassifyLoopsPass());
  if (EnableCountInstr)
    PM.add(lotus::verification::analysis::createCountInstrPass());
  if (EnableGetTestTargets)
    PM.add(lotus::verification::analysis::createGetTestTargetsPass());
  if (EnableCheckModule)
    PM.add(lotus::verification::analysis::createCheckModulePass());
  if (EnableUnrolling)
    PM.add(lotus::verification::transform::createUnrollingPass());
  if (EnableExplicitConsdes)
    PM.add(lotus::verification::transform::createExplicitConsdesPass());
  if (EnableDeleteCalls)
    PM.add(lotus::verification::transform::createDeleteCallsPass());
  if (EnableReplaceVerifierAtomic)
    PM.add(lotus::verification::transform::createReplaceVerifierAtomicPass());

  if (!Ok)
    return 1;

  PM.run(*M);

  raw_ostream &OS = Out ? Out->os() : outs();
  if (OutputAssembly) {
    M->print(OS, nullptr);
  } else {
    WriteBitcodeToFile(*M, OS);
  }

  if (Out)
    Out->keep();

  return 0;
}
