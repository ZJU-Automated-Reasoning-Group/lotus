/*
 * DyckAA Pointer Analysis Driver
 * CFL-reachability based alias analysis using fast unification.
 */

#include "Alias/AliasAnalysisWrapper/CLIUtils.h"
#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckCallGraph.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::alias::tools;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::Required);

static cl::opt<bool> PrintCallGraph("print-cg",
                                    cl::desc("Print call graph statistics"),
                                    cl::init(false));

static cl::opt<bool> Verbose("v", cl::desc("Verbose output"), cl::init(false));
static cl::opt<bool> OnlyStatistics("s", cl::desc("Only output statistics"),
                                    cl::init(false));

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "DyckAA Pointer Analysis Tool\n");

  LLVMContext Context;
  SMDiagnostic Err;

  auto M = loadIRModule(InputFilename, Context, Err, argv[0]);
  if (!M) {
    return 1;
  }

  if (Verbose) {
    errs() << "Running DyckAA on " << M->getName() << " ("
           << M->getFunctionList().size() << " functions)\n";
  }

  // Run DyckAA analysis
  legacy::PassManager PM;
  DyckAliasAnalysis *DyckAA = new DyckAliasAnalysis();
  PM.add(DyckAA);
  PM.run(*M);

  // Print call graph statistics if requested
  if (PrintCallGraph && !OnlyStatistics) {
    DyckCallGraph *CG = DyckAA->getDyckCallGraph();
    if (CG) {
      unsigned totalIndirectCalls = 0, totalTargets = 0;
      for (auto it = CG->begin(); it != CG->end(); ++it) {
        if (auto *Node = it->second) {
          totalIndirectCalls += Node->pointer_call_size();
          for (auto pc = Node->pointer_call_begin();
               pc != Node->pointer_call_end(); ++pc) {
            if (*pc)
              totalTargets += (*pc)->size();
          }
        }
      }
      outs() << "Call graph: " << CG->size() << " nodes, " << totalIndirectCalls
             << " indirect calls, " << totalTargets << " resolved targets\n";
    }
  }

  if (OnlyStatistics || Verbose) {
    errs() << "\n=== Statistics ===\n";
    PrintStatistics(errs());
  }

  return 0;
}