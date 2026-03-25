#include "Utils/Parallel/Scheduler/ParallelSchedulerPass.h"

#include "Utils/Parallel/Scheduler/PipelineScheduler.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#define DEBUG_TYPE "ParallelSchedulerPass"

using namespace llvm;

char ParallelSchedulerPass::ID = 0;
static RegisterPass<ParallelSchedulerPass>
    X(DEBUG_TYPE, "Parallel Scheduler Pass for Generic Analysis");

ParallelSchedulerPass::ParallelSchedulerPass()
    : ModulePass(ID), Name("Parallel Scheduler"), AnalysisType(1),
      EnableGC(true) {}

ParallelSchedulerPass::~ParallelSchedulerPass() {}

void ParallelSchedulerPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<CallGraphWrapperPass>();
}

bool ParallelSchedulerPass::runOnModule(Module &M) {
  if (!AnalysisCallback) {
    errs() << "Error: No analysis callback set for ParallelSchedulerPass!\n";
    return false;
  }

  llvm::outs() << "Running " << Name << " on module " << M.getName() << "\n";

  // Get call graph
  CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();

  // Determine analysis type
  PipelineScheduler::AnalysisType AT;
  switch (AnalysisType) {
  case 0:
    AT = PipelineScheduler::AT_Local;
    llvm::outs() << "Analysis type: Local\n";
    break;
  case 1:
    AT = PipelineScheduler::AT_BottomUp;
    llvm::outs() << "Analysis type: Bottom-up\n";
    break;
  case 2:
    AT = PipelineScheduler::AT_TopDown;
    llvm::outs() << "Analysis type: Top-down\n";
    break;
  default:
    AT = PipelineScheduler::AT_BottomUp;
  }

  // Create and configure scheduler
  PipelineScheduler Scheduler(M, CG, AT);
  Scheduler.setTaskCallback(AnalysisCallback);

  if (GCCallback && EnableGC) {
    Scheduler.setGCCallback(GCCallback);
  } else {
    Scheduler.setEnableGC(false);
  }

  // Run the scheduler
  Scheduler.run();

  llvm::outs() << "Parallel scheduler completed successfully!\n";

  return false; // We don't modify the module
}
