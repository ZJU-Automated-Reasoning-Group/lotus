/**
 * @file WPDSLivenessAnalysis.cpp
 * @brief Liveness analysis using WPDS-based dataflow engine
 * Author: rainoftime
 */

#include "Dataflow/WPDS/Clients/WPDSLivenessAnalysis.h"

#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using wpds::DataFlowFacts;
using wpds::GenKillTransformer;
using wpds::InterProceduralDataFlowEngine;

// Liveness is a backward analysis
// GEN: variables used (read) by instruction
// KILL: variables defined (written) by instruction

static GenKillTransformer *createLivenessTransformer(Instruction *I) {
  std::set<Value *> genSet;  // Variables used
  std::set<Value *> killSet; // Variables defined

  // Definitions kill liveness
  if (!I->getType()->isVoidTy()) {
    // This instruction defines a value
    killSet.insert(I);
  }

  // Uses generate liveness
  for (Use &U : I->operands()) {
    Value *V = U.get();
    if (isa<Instruction>(V) || isa<Argument>(V)) {
      genSet.insert(V);
    }
  }

  // Special handling for stores
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    Value *ptr = SI->getPointerOperand();
    Value *val = SI->getValueOperand();

    // Store uses the value being stored
    if (isa<Instruction>(val) || isa<Argument>(val)) {
      genSet.insert(val);
    }

    // The store address is read to determine where the write goes.
    if (isa<Instruction>(ptr) || isa<Argument>(ptr)) {
      genSet.insert(ptr);
    }
  }

  // Special handling for loads
  if (auto *LI = dyn_cast<LoadInst>(I)) {
    Value *ptr = LI->getPointerOperand();

    // Load uses the memory location
    if (isa<Instruction>(ptr) || isa<Argument>(ptr)) {
      genSet.insert(ptr);
    }

    // Load defines its result
    killSet.insert(LI);
  }

  DataFlowFacts gen(genSet);
  DataFlowFacts kill(killSet);
  return GenKillTransformer::makeGenKillTransformer(kill, gen);
}

std::unique_ptr<mono::DataFlowResult> runLivenessAnalysis(Module &module) {
  InterProceduralDataFlowEngine engine;
  std::set<Value *> initial; // Start with empty set (nothing live at exit)
  return engine.runBackwardAnalysis(module, createLivenessTransformer, initial);
}

void demoLivenessAnalysis(Module &module) {
  auto result = runLivenessAnalysis(module);

  errs() << "[WPDS][Liveness] Analysis Results:\n";
  for (auto &F : module) {
    if (F.isDeclaration())
      continue;
    errs() << "Function: " << F.getName() << "\n";

    for (auto &BB : F) {
      for (auto &I : BB) {
        const std::set<Value *> &in = result->IN(&I);
        if (!in.empty()) {
          errs() << "  Instruction: ";
          I.print(errs());
          errs() << "\n    Live-in: " << in.size() << " variables\n";
          for (auto *V : in) {
            errs() << "      ";
            if (auto *Inst = dyn_cast<Instruction>(V)) {
              errs() << Inst->getName();
            } else if (auto *Arg = dyn_cast<Argument>(V)) {
              errs() << Arg->getName();
            }
            errs() << "\n";
          }
        }
      }
    }
  }
}
