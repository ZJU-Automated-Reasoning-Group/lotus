// Sifa (Symbolic Interpretation with Fluid Abstractions) verifier tool.
// ./build/bin/sifa tmp/sifa-demo/loop.bc --log-level=progress --symabs
//  Build bitcode without optnone so mem2reg can promote allocas to SSA

#include "Verification/Sifa/Sifa.h"

#include "Verification/Sifa/Log/SifaLogger.h"
#include "Verification/Sifa/SifaSymAbs.h"
#include "Verification/SymAbsAI/Core/AbstractValue.h"
#include "Verification/SymAbsAI/Utils/PrettyPrinter.h"

#include <memory>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::sifa;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode>"),
                                          cl::value_desc("bitcode"));

static cl::opt<std::string>
    FunctionName("function", cl::desc("Function to analyze (default: main)"),
                 cl::value_desc("name"));

static cl::opt<std::string>
    BlockLabel("block", cl::desc("Block to analyze to (default: return)"),
               cl::value_desc("label"));

static cl::opt<std::string> DomainOpt("abstract-domain",
                                      cl::desc("Interval, Octagon, or both"),
                                      cl::value_desc("domain"),
                                      cl::init("Interval"));

static cl::opt<bool> UseSymAbs("symabs",
                               cl::desc("Use SMT-backed SymbolicAbstraction"),
                               cl::init(false));

static cl::opt<bool>
    RepresentAll("represent-all",
                 cl::desc("Represent all SSA values in the invariant (e.g. i, "
                          "s); otherwise only named values"),
                 cl::init(false));

static cl::opt<int> WideningDelay(
    "widening-delay",
    cl::desc("Delay before first widening in SMT loop (higher => more precise "
             "intervals); default 9999 for --symabs"),
    cl::init(-1));

static cl::opt<int> WideningFrequency(
    "widening-frequency",
    cl::desc("Widening frequency after delay (only with --symabs)"),
    cl::init(-1));

static cl::opt<bool> ReachabilityOnly("reachability",
                                      cl::desc("Only check reachability"),
                                      cl::init(false));

static cl::opt<bool> NoValidateSubset("no-validate-subset",
                                      cl::desc("Disable IR subset validation"),
                                      cl::init(false));

static cl::opt<bool> ListFunctions("list-functions", cl::desc("List functions"),
                                   cl::init(false));

static cl::opt<bool> ListBlocks("list-blocks", cl::desc("List basic blocks"),
                                cl::init(false));

static cl::opt<bool> Verbose("verbose",
                             cl::desc("Print detailed state (SymAbs)"),
                             cl::init(false));

static cl::opt<std::string>
    LogLevel("log-level",
             cl::desc("none|error|warning|info|progress|debug|trace"),
             cl::value_desc("level"), cl::init("none"));

static llvm::BasicBlock *findBlock(llvm::Function &F,
                                   const std::string &label) {
  for (auto &BB : F)
    if (BB.hasName() && BB.getName() == label)
      return &BB;
  return nullptr;
}

static SifaLogLevel parseLogLevel(const std::string &s) {
  const std::pair<std::string, SifaLogLevel> levels[] = {
      {"error", SifaLogLevel::Error}, {"warning", SifaLogLevel::Warning},
      {"info", SifaLogLevel::Info},   {"progress", SifaLogLevel::Progress},
      {"debug", SifaLogLevel::Debug}, {"trace", SifaLogLevel::Trace}};
  for (const auto &p : levels)
    if (s == p.first)
      return p.second;
  return SifaLogLevel::None;
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(
      argc, argv, "Sifa - Symbolic Interpretation with Fluid Abstractions\n");

  SifaLogLevel logLevel = parseLogLevel(LogLevel.getValue());
  if (logLevel != SifaLogLevel::None) {
    SifaLogger::setOutputStream(&errs());
    SifaLogger::setLevel(logLevel);
  }

  if (InputFilename.empty()) {
    errs() << "Error: input bitcode required.\n";
    return 1;
  }
  SifaLogger::progress("Loading bitcode: " + InputFilename.getValue());

  LLVMContext context;
  SMDiagnostic err;
  std::unique_ptr<Module> module = parseIRFile(InputFilename, err, context);
  if (!module) {
    err.print(argv[0], errs());
    return 1;
  }
  SifaLogger::progress("Module loaded");

  Function *targetFunc = FunctionName.empty()
                             ? module->getFunction("main")
                             : module->getFunction(FunctionName);
  if (!targetFunc)
    for (auto &F : *module)
      if (!F.isDeclaration()) {
        targetFunc = &F;
        break;
      }
  if (!targetFunc) {
    errs() << "Error: Function '"
           << (FunctionName.empty() ? "main" : FunctionName.getValue())
           << "' not found\n";
    return 1;
  }

  if (ListFunctions) {
    for (auto &F : *module)
      if (!F.isDeclaration())
        outs() << "  " << F.getName() << "\n";
    return 0;
  }
  if (ListBlocks) {
    for (auto &BB : *targetFunc)
      outs() << "  " << (BB.hasName() ? BB.getName() : "(unnamed)") << "\n";
    return 0;
  }

  SifaOptions sifaOpts;
  try {
    if (UseSymAbs) {
      SifaSymAbsOptions options;
      options.abstractDomain = DomainOpt.getValue();
      options.representAllInstructions = RepresentAll;
      options.wideningDelay = WideningDelay.getNumOccurrences()
                                  ? WideningDelay
                                  : (UseSymAbs ? 9999 : -1);
      options.wideningFrequency = WideningFrequency;
      options.validateLlvmSubset = !NoValidateSubset;
      options.logLevel = logLevel;
      if (logLevel >= SifaLogLevel::Progress)
        options.progressStream = &errs();

      unsigned nBlocks = 0;
      for (auto &BB : *targetFunc)
        (void)BB, ++nBlocks;
      outs() << "Abstract domain(s): " << options.abstractDomain << "\n";
      SifaLogger::progress("Analyzing (SymAbs/SMT) function '" +
                           targetFunc->getName().str() + "' (" +
                           std::to_string(nBlocks) +
                           " blocks, domain: " + options.abstractDomain + ")");

      auto reportSymAbs = [&](const std::string &label, SymAbsState state) {
        if (ReachabilityOnly) {
          bool reachable = state && !state->isBottom();
          outs() << label
                 << (reachable ? " is reachable\n" : " is not reachable\n");
          return reachable ? 0 : 1;
        }
        if (!state || state->isBottom()) {
          outs() << label << ": "
                 << (!state ? "unreachable (bottom)" : "bottom") << "\n";
          return 0;
        }
        outs() << label << ": state (top/non-bottom)\n";
        outs() << "Final invariant:\n";
        symabs_ai::PrettyPrinter pp(/*output_html=*/Verbose);
        state->prettyPrint(pp);
        outs() << pp.str() << "\n";
        return 0;
      };

      if (!BlockLabel.empty()) {
        BasicBlock *targetBlock = findBlock(*targetFunc, BlockLabel.getValue());
        if (!targetBlock) {
          errs() << "Error: block '" << BlockLabel.getValue()
                 << "' not found\n";
          return 1;
        }
        SymAbsState state =
            analyzeSymAbsTo(*module, *targetFunc, *targetBlock, options);
        int ret = reportSymAbs("Block " + BlockLabel.getValue(), state);
        if (ret != 0)
          return ret;
      } else {
        int ret = reportSymAbs(
            "Return", analyzeSymAbsToReturn(*module, *targetFunc, options));
        if (ret != 0)
          return ret;
      }
    } else {
      bool useOctagon =
          DomainOpt.getValue().find("Octagon") != std::string::npos;
      unsigned nBlocks = 0;
      for (auto &BB : *targetFunc)
        (void)BB, ++nBlocks;
      SifaLogger::progress(
          "Analyzing (instruction transfer, no SMT) function '" +
          targetFunc->getName().str() + "' (" + std::to_string(nBlocks) +
          " blocks, domain: " + (useOctagon ? "Octagon" : "Interval") + ")");

      if (!BlockLabel.empty() &&
          !findBlock(*targetFunc, BlockLabel.getValue())) {
        errs() << "Error: block '" << BlockLabel.getValue() << "' not found\n";
        return 1;
      }

      auto label = BlockLabel.empty() ? std::string("Return")
                                      : "Block " + BlockLabel.getValue();
      auto reportNative = [&](bool bottom, auto &state, const char *dom) {
        if (bottom) {
          outs() << label << ": unreachable (bottom)\n";
          return;
        }
        if (ReachabilityOnly) {
          outs() << label << " is reachable\n";
          return;
        }
        outs() << label << ": state (" << dom << " domain)\n";
        state.print(outs());
      };

      if (useOctagon) {
        OctagonState state =
            BlockLabel.empty()
                ? analyzeToReturnWithOctagonDomain(
                      *targetFunc, OctagonState(false), sifaOpts)
                : analyzeToWithOctagonDomain(
                      *targetFunc,
                      *findBlock(*targetFunc, BlockLabel.getValue()),
                      OctagonState(false), sifaOpts);
        OctagonDomain dom(nullptr, nullptr);
        reportNative(dom.isBottom(state), state, "octagon");
      } else {
        IntervalState state =
            BlockLabel.empty()
                ? analyzeToReturnWithIntervalDomain(
                      *targetFunc, IntervalState(false), sifaOpts)
                : analyzeToWithIntervalDomain(
                      *targetFunc,
                      *findBlock(*targetFunc, BlockLabel.getValue()),
                      IntervalState(false), sifaOpts);
        IntervalDomain dom(nullptr, nullptr);
        reportNative(dom.isBottom(state), state, "interval");
      }
    }
    outs() << "Sifa analysis completed successfully.\n";
  } catch (const std::exception &e) {
    errs() << "Error during analysis: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
