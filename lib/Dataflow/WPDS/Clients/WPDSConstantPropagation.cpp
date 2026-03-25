/**
 * @file WPDSConstantPropagation.cpp
 * @brief Constant propagation analysis using WPDS-based dataflow engine
 *        Actually implements "Varying Analysis" - values NOT in the set are
 * Constant. Author: rainoftime
 */

#include "Dataflow/WPDS/Clients/WPDSConstantPropagation.h"

#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using wpds::DataFlowFacts;
using wpds::GenKillTransformer;
using wpds::InterProceduralDataFlowEngine;
using wpds::MemoryObjectFact;

// Coarse may-style demo:
// The set tracks "varying" SSA values and pointer-typed memory proxies.
// This is not a precise memory-aware constant propagation framework.

static GenKillTransformer *
createConstantPropagationTransformer(Instruction *I) {
  std::set<Value *> genSet;
  std::set<Value *> killSet;
  std::map<Value *, DataFlowFacts> flowMap;

  auto addFlow = [&](Value *src, Value *dst) {
    if (!flowMap.count(src)) {
      flowMap[src] = DataFlowFacts::EmptySet();
    }
    flowMap[src].addFact(dst);
  };

  // 1. Sources of Variation
  if (auto *AI = dyn_cast<AllocaInst>(I)) {
    // Memory content is initially garbage (varying)
    MemoryObjectFact::addRepresentativeFact(genSet, AI);
  } else if (auto *CI = dyn_cast<CallBase>(I)) {
    // Call result is varying (unless we know the function is const/pure)
    if (!CI->getType()->isVoidTy()) {
      genSet.insert(CI);
    }
  }

  // 2. Kill / Flow
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    Value *val = SI->getValueOperand();
    Value *ptr = MemoryObjectFact::getRepresentative(SI->getPointerOperand());

    // Storing to ptr overwrites previous content.
    // So we KILL the "varying" status of ptr (memory).
    if (ptr != nullptr) {
      killSet.insert(ptr);
    }

    // If 'val' is varying, it flows to 'ptr'.
    // If 'val' is constant (not in set), 'ptr' remains killed (constant).
    if (ptr != nullptr) {
      addFlow(val, ptr);
    }
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    // val = load ptr
    // If memory at ptr is varying, val is varying.
    if (Value *ptr = MemoryObjectFact::getRepresentative(LI->getPointerOperand())) {
      addFlow(ptr, LI);
    }
  } else if (auto *BI = dyn_cast<BinaryOperator>(I)) {
    // z = x + y
    // If x varying OR y varying -> z varying.
    // Flow: x -> z, y -> z.
    addFlow(BI->getOperand(0), I);
    addFlow(BI->getOperand(1), I);
  } else if (auto *PHI = dyn_cast<PHINode>(I)) {
    for (Value *inc : PHI->incoming_values()) {
      addFlow(inc, I);
    }
  } else if (auto *CI = dyn_cast<CastInst>(I)) {
    addFlow(CI->getOperand(0), I);
  } else if (auto *GEPI = dyn_cast<GetElementPtrInst>(I)) {
    // For ConstProp, GEP calculation is constant if base and indices are
    // constant. flow base -> gep flow indices -> gep
    addFlow(GEPI->getPointerOperand(), I);
    if (Value *ptr = MemoryObjectFact::getRepresentative(GEPI->getPointerOperand())) {
      addFlow(ptr, I);
    }
    for (auto &idx : GEPI->indices()) {
      addFlow(idx.get(), I);
    }
  } else if (auto *SI = dyn_cast<SelectInst>(I)) {
    // Cond varying -> Result varying (Control dependence)
    // True/False varying -> Result varying
    addFlow(SI->getCondition(), I);
    addFlow(SI->getTrueValue(), I);
    addFlow(SI->getFalseValue(), I);
  }

  // Constants (Literals)
  // They are not Instructions, they are Values.
  // They are never in the Set.
  // So implicit logic handles them correctly.

  DataFlowFacts gen(genSet);
  DataFlowFacts kill(killSet);
  return GenKillTransformer::makeGenKillTransformer(kill, gen, flowMap);
}

std::unique_ptr<mono::DataFlowResult>
runConstantPropagationAnalysis(Module &module) {
  InterProceduralDataFlowEngine engine;
  std::set<Value *> initial;

  // Arguments of main are varying
  for (auto &F : module) {
    if (F.getName() == "main") {
      for (auto &Arg : F.args()) {
      initial.insert(&Arg);
      if (Arg.getType()->isPointerTy()) {
        MemoryObjectFact::addRepresentativeFact(initial, &Arg);
      }
    }
  }
  }
  // Also Global Variables? Mutable globals are varying initially?
  // Or assume 0-initialized (Constant).
  // If external, they are varying.
  for (auto &G : module.globals()) {
    if (!G.hasInitializer() && G.hasExternalLinkage()) {
      initial.insert(&G);
      MemoryObjectFact::addRepresentativeFact(initial, &G);
    }
  }

  return engine.runForwardAnalysis(module, createConstantPropagationTransformer,
                                   initial);
}

void demoConstantPropagationAnalysis(Module &module) {
  auto result = runConstantPropagationAnalysis(module);

  errs()
      << "[WPDS][ConstantProp] Analysis Results (Values Provably Constant):\n";
  for (auto &F : module) {
    if (F.isDeclaration())
      continue;
    errs() << "Function: " << F.getName() << "\n";

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (I.getType()->isVoidTy())
          continue;

        const std::set<Value *> &out = result->OUT(&I);

        // If I is NOT in OUT set, it is Constant
        // (Assuming it's not Dead/Unreachable - standard DF assumes all
        // reachable)
        if (out.find(&I) == out.end()) {
          errs() << "  Constant: ";
          I.print(errs());
          errs() << "\n";
        }
      }
    }
  }
}
