// ExtendedCallGraph using DyckAA for indirect call resolution
#include "Fuzzing/Analysis/ExtendedCallGraph.h"

#include "Alias/DyckAA/DyckAliasAnalysis.h"
#include "Alias/DyckAA/DyckCallGraph.h"
#include "Alias/DyckAA/DyckCallGraphNode.h"

#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LegacyPassManager.h>

using namespace llvm;

AnalysisKey ExtendedCallGraphAnalysis::Key;

ExtendedCallGraphAnalysis::Result
ExtendedCallGraphAnalysis::run(Module &M, ModuleAnalysisManager &) {
  // Build a basic LLVM call graph
  auto LLVMCallGraph = CallGraph(M);

  // Run DyckAA to get indirect call resolution
  legacy::PassManager PM;
  auto *DyckAAPass = new DyckAliasAnalysis();
  PM.add(DyckAAPass);
  PM.run(M);

  // Get the DyckCallGraph with resolved indirect calls
  auto *DyckCG = DyckAAPass->getDyckCallGraph();

  // Iterate through all functions in the DyckCallGraph
  for (auto NodeIt = DyckCG->nodes_begin(); NodeIt != DyckCG->nodes_end(); ++NodeIt) {
    auto *DyckNode = *NodeIt;
    auto *Caller = DyckNode->getLLVMFunction();
    
    if (!Caller || Caller->isDeclaration()) {
      continue;
    }

    auto *LLVMCallerNode = LLVMCallGraph[Caller];

    // Process all pointer (indirect) calls in this function
    for (auto PCIt = DyckNode->pointer_call_begin(); 
         PCIt != DyckNode->pointer_call_end(); ++PCIt) {
      auto *PC = *PCIt;
      auto *CallInst = dyn_cast<CallBase>(PC->getInstruction());
      
      if (!CallInst) {
        continue;
      }

      // Add edges for all resolved callees of this indirect call
      for (auto *Callee : *PC) {
        if (!Callee || Callee->isDeclaration()) {
          continue;
        }

        // Add the resolved indirect call edge to the LLVM call graph
        LLVMCallerNode->addCalledFunction(
            const_cast<CallBase *>(CallInst),
            LLVMCallGraph[Callee]);
      }
    }
  }

  return LLVMCallGraph;
}