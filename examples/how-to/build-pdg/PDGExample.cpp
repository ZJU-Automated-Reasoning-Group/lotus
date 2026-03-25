//===-- PDGExample.cpp - Example code for using Program Dependency Graph  ----===//
//
// This file demonstrates how to use the PDG (Program Dependency Graph) library
// to analyze LLVM IR code dependencies.
//
//===----------------------------------------------------------------------===//

#include "IR/PDG/Core/ProgramDependencyGraph.h"
#include "IR/PDG/Core/ControlDependencyGraph.h"
#include "IR/PDG/Core/DataDependencyGraph.h"
#include "IR/PDG/Core/PDGCallGraph.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace pdg;

static cl::opt<std::string> InputFilename(cl::Positional,
                                          cl::desc("<input bitcode file>"),
                                          cl::init("-"),
                                          cl::value_desc("filename"));

static cl::opt<std::string> TargetFunction("function",
                                          cl::desc("Specify function to analyze in detail"),
                                          cl::value_desc("function name"));

// Simplified edge type string function
std::string getEdgeTypeString(EdgeType type) {
  switch (type) {
    case EdgeType::DATA_DEF_USE: return "Data Def-Use";
    case EdgeType::DATA_RAW: return "Data RAW";
    case EdgeType::DATA_ALIAS: return "Data Alias";
    case EdgeType::CONTROLDEP_ENTRY: return "Control Entry";
    case EdgeType::CONTROLDEP_BR: return "Control Branch";
    default: return "Other";
  }
}

// Simplified node print function
void printNode(Node* node) {
  if (!node) return;
  
  Value* val = node->getValue();
  if (!val) {
    outs() << "Node with null value";
    return;
  }
  
  if (Function* func = dyn_cast<Function>(val)) {
    outs() << "Function: " << func->getName();
  } else if (Instruction* inst = dyn_cast<Instruction>(val)) {
    std::string str;
    raw_string_ostream OS(str);
    inst->print(OS);
    outs() << OS.str();
  } else {
    outs() << *val;
  }
}

int main(int argc, char **argv) {
  // Parse command line arguments
  cl::ParseCommandLineOptions(argc, argv, "PDG Example\n");

  // Create an LLVM context and source manager
  LLVMContext Context;
  SMDiagnostic Err;

  // Load the input module
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  outs() << "Successfully loaded module: " << InputFilename << "\n";
  
  // Build the PDG and Call Graph
  ProgramGraph &PDG = ProgramGraph::getInstance();
  PDG.build(*M);
  PDG.bindDITypeToNodes(*M);
  
  PDGCallGraph callGraph;
  callGraph.build(*M);
  outs() << "Call Graph:\n";
  callGraph.dump();
  
  // Count PDG nodes
  unsigned nodeCount = 0;
  for (auto node_iter = PDG.begin(); node_iter != PDG.end(); ++node_iter) {
    nodeCount++;
  }
  outs() << "PDG contains " << nodeCount << " nodes\n";
  
  // List functions
  outs() << "Functions in the module:\n";
  for (auto &F : *M) {
    if (F.isDeclaration() || F.empty())
      continue;
    
    if (PDG.hasFuncWrapper(F)) {
      outs() << "  - " << F.getName() << " (arguments: " << F.arg_size() << ")\n";
    }
  }
  
  // Count call sites
  unsigned callSiteCount = 0;
  for (auto &F : *M) {
    if (F.isDeclaration() || F.empty() || !PDG.hasFuncWrapper(F))
      continue;
    
    FunctionWrapper *funcWrapper = PDG.getFuncWrapper(F);
    auto callInsts = funcWrapper->getCallInsts();
    
    for (auto *ci : callInsts) {
      callSiteCount++;
      Function *calledFunc = pdgutils::getCalledFunc(*ci);
      outs() << "  Call from " << F.getName() << " to " 
             << (calledFunc ? calledFunc->getName() : "indirect call") << "\n";
    }
  }
  outs() << "Found " << callSiteCount << " call sites in the module\n";
  
  // Analyze target function if specified
  if (!TargetFunction.empty()) {
    Function *targetFunc = M->getFunction(TargetFunction);
    if (!targetFunc || targetFunc->isDeclaration()) {
      outs() << "Target function '" << TargetFunction << "' not found or is a declaration\n";
    } else {
      outs() << "\nAnalyzing function: " << TargetFunction << "\n";
      
      // Map instructions to nodes
      std::map<Instruction*, Node*> instNodeMap;
      for (auto &BB : *targetFunc) {
        for (auto &I : BB) {
          Node *node = PDG.getNode(I);
          if (node) instNodeMap[&I] = node;
        }
      }
      
      // Print key dependencies
      for (auto &pair : instNodeMap) {
        Instruction *inst = pair.first;
        Node *node = pair.second;
        
        if (isa<CallInst>(inst) || isa<LoadInst>(inst) || isa<StoreInst>(inst) || 
            isa<BranchInst>(inst) || isa<ReturnInst>(inst)) {
          outs() << "Instruction: ";
          inst->print(outs());
          outs() << "\n";
          
          if (!node->getOutEdgeSet().empty()) {
            outs() << "  Dependencies:\n";
            for (auto *edge : node->getOutEdgeSet()) {
              outs() << "    - " << getEdgeTypeString(edge->getEdgeType()) << ": ";
              printNode(edge->getDstNode());
              outs() << "\n";
            }
            outs() << "\n";
          }
        }
      }
    }
  }
  
  return 0;
} 

