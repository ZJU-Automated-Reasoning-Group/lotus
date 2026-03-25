/*
An Inclusion-based, Semi-Sparse, Flow- and Context-Sensitive Pointer Analysis
Tool. Cmd options: -ext <file>: External pointer table file for modeling library
functions -no-prepass: Skip TPA IR normalization prepasses (GEP expansion, etc.)
-prepass-out <file>: Write module after prepass to file (suffix .ll or .bc)
-cfg-dot-dir <dir>: Write per-function pointer CFGs as .dot files into directory
-print-pts: Print points-to sets for pointers that were materialized by the
analysis -print-indirect-calls: Print resolved targets for each indirect call in
the module -k-limit <n>: Set k-limit for context-sensitive analysis (0 =
context-insensitive, default: 0) -context-strategy <klimit|selective>: klimit =
k-CFA at all calls; selective = 0-CFA at direct calls, k-CFA at indirect
(default: klimit)
*/

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/TPA/Context/ContextPolicy.h"
#include "Alias/TPA/Context/KLimitContext.h"
#include "Alias/TPA/PointerAnalysis/Analysis/SemiSparsePointerAnalysis.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/SemiSparseProgramBuilder.h"
#include "Alias/TPA/Transforms/RunPrepass.h"
#include "Alias/TPA/Util/IO/PointerAnalysis/Printer.h"
#include "Alias/TPA/Util/IO/PointerAnalysis/WriteDotFile.h"
#include "Alias/TPA/Util/Log.h"
#include "Utils/LLVM/IO/WriteIR.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;

// Command line options
static cl::opt<std::string> InputFile(cl::Positional,
                                      cl::desc("<input bitcode file>"),
                                      cl::value_desc("filename"));

static cl::opt<std::string>
    ExtPointerTableFile("ext",
                        cl::desc("External pointer table file (optional)"),
                        cl::value_desc("filename"), cl::init(""));

static cl::opt<bool> NoPrepass("no-prepass",
                               cl::desc("Skip TPA IR normalization prepasses"),
                               cl::init(false));

static cl::opt<std::string> PrepassOutFile(
    "prepass-out",
    cl::desc("Write module after prepass to this file (suffix .ll or .bc)"),
    cl::value_desc("filename"), cl::init(""));

static cl::opt<std::string> CFGDotOutDir(
    "cfg-dot-dir",
    cl::desc(
        "Write per-function pointer CFGs as .dot files into this directory"),
    cl::value_desc("directory"), cl::init(""));

static cl::opt<bool> PrintPts("print-pts",
                              cl::desc("Print points-to sets for pointers that "
                                       "were materialized by the analysis"),
                              cl::init(false));

static cl::opt<bool> PrintIndirectCalls(
    "print-indirect-calls",
    cl::desc("Print resolved targets for each indirect call in the module"),
    cl::init(false));

static cl::opt<unsigned>
    KLimit("k-limit",
           cl::desc("Set k-limit for context-sensitive analysis (0 = "
                    "context-insensitive, default: 0)"),
           cl::init(0));

static cl::opt<std::string> ContextStrategyOpt(
    "context-strategy",
    cl::desc("Context strategy: klimit (k-CFA at all calls) or selective "
             "(0-CFA at direct calls, k-CFA at indirect calls)"),
    cl::init("klimit"));

static std::string findDefaultPointerSpec() {
  // 1) LOTUS_CONFIG_DIR/ptr.spec
  if (const char *envPath = std::getenv("LOTUS_CONFIG_DIR")) {
    SmallString<256> candidate(envPath);
    sys::path::append(candidate, "ptr.spec");
    if (sys::fs::exists(candidate))
      return candidate.str().str();
  }

  // 2) <cwd>/config/ptr.spec
  SmallString<256> cwd;
  if (!sys::fs::current_path(cwd)) {
    SmallString<256> inCwd = cwd;
    sys::path::append(inCwd, "config", "ptr.spec");
    if (sys::fs::exists(inCwd))
      return inCwd.str().str();

    // 3) <parent of cwd>/config/ptr.spec
    SmallString<256> parent = cwd;
    sys::path::remove_filename(parent);
    sys::path::append(parent, "config", "ptr.spec");
    if (sys::fs::exists(parent))
      return parent.str().str();
  }

  // Fallback to relative path for backward compatibility
  return "config/ptr.spec";
}

static void
collectCandidatePointerValues(const Module &M,
                              std::unordered_set<const Value *> &out) {
  for (const GlobalVariable &GV : M.globals())
    out.insert(&GV);

  for (const Function &F : M) {
    for (const Argument &A : F.args())
      if (A.getType()->isPointerTy())
        out.insert(&A);

    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        if (I.getType()->isPointerTy())
          out.insert(&I);
      }
    }
  }
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  cl::ParseCommandLineOptions(
      argc, argv,
      "TPA (flow-/context-sensitive semi-sparse pointer analysis) tool\n");

  // Initialize spdlog with default pattern
  spdlog::set_pattern("%^[%l]%$ %v");

  LOG_INFO("Loading LLVM IR from: {}", InputFile);
  LLVMContext Context;
  SMDiagnostic Err;
  auto M = loadIRModule(InputFile, Context, Err, argv[0]);
  if (!M) {
    LOG_ERROR("Failed to parse input file: {}", InputFile);
    return 1;
  }
  LOG_INFO("Module loaded: {} functions, {} global variables",
           M->getFunctionList().size(), M->getGlobalList().size());

  if (!NoPrepass) {
    LOG_INFO("Running TPA IR normalization prepasses...");
    transform::runPrepassOn(*M);
    LOG_INFO("Prepass completed");
  } else {
    LOG_INFO("Skipping prepass (--no-prepass specified)");
  }

  if (!PrepassOutFile.empty()) {
    const bool isText = StringRef(PrepassOutFile).endswith_insensitive(".ll");
    LOG_INFO("Writing prepass output to: {}", PrepassOutFile);
    util::io::writeModuleToFile(*M, PrepassOutFile.c_str(), isText);
  }

  // Set context strategy and k-limit
  if (ContextStrategyOpt == "selective") {
    context::setContextStrategy(context::ContextStrategy::Selective);
    context::KLimitContext::setLimit(KLimit);
    if (KLimit > 0) {
      LOG_INFO(
          "Selective context: 0-CFA at direct calls, {}-CFA at indirect calls",
          KLimit);
    } else {
      LOG_INFO("Selective context: 0-CFA at direct calls, context-insensitive "
               "at indirect");
    }
  } else {
    context::setContextStrategy(context::ContextStrategy::KLimit);
    context::KLimitContext::setLimit(KLimit);
    if (KLimit > 0) {
      LOG_INFO("Context-sensitive analysis enabled with k-limit: {}", KLimit);
    } else {
      LOG_INFO("Context-insensitive analysis mode");
    }
  }

  // Build semi-sparse program and run analysis
  LOG_INFO("Building semi-sparse program representation...");
  tpa::SemiSparseProgramBuilder builder;
  tpa::SemiSparseProgram ssProg = builder.runOnModule(*M);
  // LOG_INFO("Semi-sparse program built: {} CFGs", ssProg.cfgMap.size());

  tpa::SemiSparsePointerAnalysis analysis;
  std::string pointerSpecPath = ExtPointerTableFile.empty()
                                    ? findDefaultPointerSpec()
                                    : std::string(ExtPointerTableFile);
  if (!sys::fs::exists(pointerSpecPath)) {
    LOG_ERROR("Pointer spec file not found: {}", pointerSpecPath);
    return 1;
  }
  LOG_INFO("Loading external pointer table from: {}", pointerSpecPath);
  analysis.loadExternalPointerTable(pointerSpecPath.c_str());

  LOG_INFO("Starting TPA pointer analysis...");
  analysis.runOnProgram(ssProg);
  LOG_INFO("TPA analysis completed successfully");

  if (!CFGDotOutDir.empty()) {
    std::error_code EC = sys::fs::create_directories(CFGDotOutDir);
    if (EC) {
      LOG_ERROR("Failed to create directory {}: {}", CFGDotOutDir,
                EC.message());
      return 2;
    }

    LOG_INFO("Writing CFG dot files to: {}", CFGDotOutDir);
    for (const tpa::CFG &cfg : ssProg) {
      const auto &F = cfg.getFunction();
      std::string outPath = CFGDotOutDir + "/" + F.getName().str() + ".dot";
      util::io::writeDotFile(outPath.c_str(), cfg);
    }
  }

  if (PrintIndirectCalls) {
    LOG_INFO("=== Indirect call targets ===");
    for (const Function &F : *M) {
      if (F.isDeclaration())
        continue;

      for (const BasicBlock &BB : F) {
        for (const Instruction &I : BB) {
          const auto *CB = dyn_cast<CallBase>(&I);
          if (!CB)
            continue;
          if (CB->getCalledFunction() != nullptr)
            continue;

          auto targets = analysis.getCallees(&I);
          std::string targetNames;
          for (const Function *TF : targets) {
            if (!targetNames.empty())
              targetNames += " ";
            targetNames += TF->getName().str();
          }
          std::string instStr;
          raw_string_ostream instOS(instStr);
          instOS << I;
          instOS.flush();
          LOG_INFO("{}: {} -> targets({}): {}", F.getName(), instStr,
                   targets.size(), targetNames);
        }
      }
    }
  }

  if (PrintPts) {
    LOG_INFO("=== Points-to sets ===");

    std::unordered_set<const Value *> values;
    collectCandidatePointerValues(*M, values);

    const auto &PM = analysis.getPointerManager();
    for (const Value *V : values) {
      auto ptrs = PM.getPointersWithValue(V->stripPointerCasts());
      if (ptrs.empty())
        continue;

      std::string valueStr;
      raw_string_ostream valueOS(valueStr);
      util::io::dumpValue(valueOS, *V);
      valueOS.flush();

      for (const tpa::Pointer *P : ptrs) {
        std::string ptrStr;
        raw_string_ostream ptrOS(ptrStr);
        ptrOS << *P;
        ptrOS.flush();

        std::string ptsStr;
        raw_string_ostream ptsOS(ptsStr);
        ptsOS << analysis.getPtsSet(P);
        ptsOS.flush();

        LOG_INFO("Value: {} -> {} -> {}", valueStr, ptrStr, ptsStr);
      }
    }
  }

  return 0;
}
