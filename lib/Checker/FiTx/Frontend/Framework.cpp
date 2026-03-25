// FiTx framework pass: typestate-based daily development-friendly bug checkers.
// Based on: Suzuki et al., "Balancing Analysis Time and Bug Detection: Daily
// Development-friendly Bug Detection in Linux", USENIX ATC 2024 (Section 4, 5).
#include "llvm/ADT/APFloat.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueSymbolTable.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

// include STL
#include "Checker/FiTx/Core/BasicBlock.h"
#include "Checker/FiTx/Core/Function.h"
#include "Checker/FiTx/Core/Logs.h"
#include "Checker/FiTx/Core/Value.h"
#include "Checker/FiTx/Core/ValueTypeAlias.h"
#include "Checker/FiTx/Framework_IR/IRGenerator.h"
#include "Checker/FiTx/Frontend/Analyzer.h"
#include "Checker/FiTx/Frontend/Framework.h"
#include "Checker/FiTx/Frontend/State.h"
#include "Checker/FiTx/Frontend/StateTransition.h"
#include "Checker/FiTx/Frontend/Utils.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <iterator>
#include <map>
#include <queue>
#include <set>
#include <stack>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

static llvm::cl::opt<bool>
    Async("async", llvm::cl::desc("Asynchronously conduct analysis"));

static llvm::cl::opt<bool> MeasureTime("measure",
                                       llvm::cl::desc("Measure analysis time"));

namespace fitx {

struct AnalyzerInfo {
  Analyzer *inner_analyzer;
  pid_t process_id;

  AnalyzerInfo(Analyzer *analyzer) : inner_analyzer(analyzer) {}

  void start_analyzer_process() {
    process_id = fork();
    if (process_id == 0) {
      run_analyzer();
      exit(0);
    }
  }

  void run_analyzer() { inner_analyzer->analyze(); }
};

FrameworkPass::FrameworkPass() : ModulePass(ID) {}

void FrameworkPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<ir_generator::IRGenerator>();
}

// Entry: each subclass overrides defineStates() to register StateManagers;
// we create one Analyzer per manager and run analyze() (CFG, typestate,
// summaries).
bool FrameworkPass::runOnModule(llvm::Module &M) {
  std::chrono::system_clock::time_point start, end;
  LoggingServer server;

  start = std::chrono::system_clock::now();
  defineStates(); // Detectors (UAF, Leak, etc.) add StateManagers here.

  // Create analyzers and spawn threads
  std::vector<AnalyzerInfo> analyzers;
  for (fitx::StateManager &manager : manager_) {
    LoggingClient *client = new LoggingClient();
    AnalyzerInfo info =
        AnalyzerInfo(new fitx::Analyzer(M, manager, *client));
    analyzers.push_back(info);
    server.addClient(client);
  }

  for (auto analyzer = analyzers.begin() + 1; analyzer != analyzers.end();
       analyzer++) {
    analyzer->start_analyzer_process();
  }

  // Start the first process here
  analyzers.begin()->run_analyzer();

  // Wait until the processes are done
  for (int i = 0; !Async && i < analyzers.size() - 1; i++) {
    wait(nullptr);
  }

  end = std::chrono::system_clock::now();
  server.printClientLogs();

  if (MeasureTime) {
    llvm::errs() << "[Elapsed Calculated] (" << M.getName() << ") "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(end -
                                                                          start)
                        .count()
                 << "\n";
  }

  return false;
}
} // namespace fitx

char fitx::FrameworkPass::ID = 0;

static llvm::RegisterPass<fitx::FrameworkPass>
    X("framework", "framework", false /* Only looks at CFG */,
      false /* Analysis Pass */);

static void registerFrameworkPass(const llvm::PassManagerBuilder &,
                                  llvm::legacy::PassManagerBase &PM) {
  for (auto &analysis_pass : fitx::FrameworkPass::passes)
    PM.add(analysis_pass);
}

static llvm::RegisterStandardPasses
    RegisterMyPass(llvm::PassManagerBuilder::EP_OptimizerLast,
                   registerFrameworkPass);

static llvm::RegisterStandardPasses
    RegisterMyPass1(llvm::PassManagerBuilder::EP_EnabledOnOptLevel0,
                    registerFrameworkPass);
