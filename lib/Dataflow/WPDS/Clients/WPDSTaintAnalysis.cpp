/**
 * @file WPDSTaintAnalysis.cpp
 * @brief Taint analysis using WPDS-based dataflow engine
 * Author: rainoftime
 */

#include "Dataflow/WPDS/Clients/WPDSTaintAnalysis.h"

#include "Dataflow/Mono/Support/Result.h"
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using wpds::DataFlowFacts;
using wpds::GenKillTransformer;
using wpds::InterProceduralDataFlowEngine;
using wpds::MemoryObjectFact;

// Taint analysis tracks which SSA values may be tainted. This is a coarse
// demonstration over Value* facts: external calls are approximated
// conservatively and memory/aliasing updates are not modeled precisely.

static bool isTaintSource(Instruction *I) {
  if (auto *CI = dyn_cast<CallBase>(I)) {
    if (Function *F = CI->getCalledFunction()) {
      StringRef name = F->getName();
      // Expanded list of taint sources
      if (name.contains("input") || name.contains("read") ||
          name.contains("recv") || name.contains("scanf") ||
          name.contains("getenv") || name.contains("gets") ||
          name.contains("fgets") || name.contains("fread") ||
          name.contains("socket") || name.contains("listen") ||
          name.contains("accept")) {
        return true;
      }
    }
  }
  return false;
}

static bool isSanitizer(Instruction *I) {
  if (auto *CI = dyn_cast<CallBase>(I)) {
    if (Function *F = CI->getCalledFunction()) {
      StringRef name = F->getName();
      // Common sanitizers
      if (name.contains("sanitize") || name.contains("escape") ||
          name.contains("validate") || name.contains("check_") ||
          name.contains("auth")) {
        return true;
      }
    }
  }
  return false;
}

static GenKillTransformer *createTaintTransformer(Instruction *I) {
  std::set<Value *> genSet;
  std::set<Value *> killSet;
  std::map<Value *, DataFlowFacts> flowMap;

  // 1. Taint Generation (Sources)
  if (isTaintSource(I)) {
    if (!I->getType()->isVoidTy()) {
      genSet.insert(I);
    }
  }

  // 2. Taint Sanitization (Sinks/Cleaners)
  // If a value is passed to a sanitizer, does it untaint the value?
  // Usually sanitizers return a clean value or clean the buffer.
  // Model: Output of sanitizer is clean (implicitly not in gen).
  // If sanitizer modifies memory (e.g. sanitize(buf)), we might want to kill
  // 'buf'.
  if (isSanitizer(I)) {
    for (Use &U : I->operands()) {
      Value *V = U.get();
      if (isa<Instruction>(V) || isa<Argument>(V)) {
        killSet.insert(V);
        if (V->getType()->isPointerTy()) {
          MemoryObjectFact::addRepresentativeFact(killSet, V);
        }
      }
    }
  }

  // 3. Taint Propagation (Flow)

  // Helper to add flow x -> y
  auto addFlow = [&](Value *src, Value *dst) {
    if (!flowMap.count(src)) {
      flowMap[src] = DataFlowFacts::EmptySet();
    }
    flowMap[src].addFact(dst);
  };

  if (auto *SI = dyn_cast<StoreInst>(I)) {
    // store val, ptr
    // val flows to the canonical memory object
    Value *val = SI->getValueOperand();
    MemoryObjectFact::addFlow(flowMap, val, SI->getPointerOperand());
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    // val = load ptr; the canonical memory object flows to val.
    MemoryObjectFact::addFlow(flowMap, LI->getPointerOperand(), I);
  } else if (auto *PHI = dyn_cast<PHINode>(I)) {
    // phi(v1, v2)
    // v1 -> phi, v2 -> phi
    for (Value *inc : PHI->incoming_values()) {
      addFlow(inc, I);
    }
  } else if (auto *BI = dyn_cast<BinaryOperator>(I)) {
    // res = op(v1, v2)
    // v1 -> res, v2 -> res
    addFlow(BI->getOperand(0), I);
    addFlow(BI->getOperand(1), I);
  } else if (auto *CI = dyn_cast<CastInst>(I)) {
    addFlow(CI->getOperand(0), I);
  } else if (auto *GEPI = dyn_cast<GetElementPtrInst>(I)) {
    addFlow(GEPI->getPointerOperand(), I);
    MemoryObjectFact::addFlow(flowMap, GEPI->getPointerOperand(), I);
  } else if (auto *SI = dyn_cast<SelectInst>(I)) {
    // select(c, v1, v2)
    // v1 -> res, v2 -> res. (ignoring cond taint for now)
    addFlow(SI->getTrueValue(), I);
    addFlow(SI->getFalseValue(), I);
  } else if (auto *CI = dyn_cast<CallBase>(I)) {
    // Handle intrinsic calls like memcpy?
    if (auto *MI = dyn_cast<MemCpyInst>(I)) {
      // memcpy(dest, src, len)
      MemoryObjectFact::addFlow(flowMap, MI->getSource(), MI->getDest());
    } else if (auto *MMI = dyn_cast<MemMoveInst>(I)) {
      MemoryObjectFact::addFlow(flowMap, MMI->getSource(), MMI->getDest());
    }
  }

  DataFlowFacts gen(genSet);
  DataFlowFacts kill(killSet);
  return GenKillTransformer::makeGenKillTransformer(kill, gen, flowMap);
}

std::unique_ptr<mono::DataFlowResult> runTaintAnalysis(Module &module) {
  InterProceduralDataFlowEngine engine;
  std::set<Value *> initial;

  // Mark main arguments as tainted (common assumption for CLI/CGI apps)
  for (auto &F : module) {
    if (F.getName() == "main") {
      for (auto &Arg : F.args()) {
        initial.insert(&Arg);
      }
    }
  }

  return engine.runForwardAnalysis(module, createTaintTransformer, initial);
}

void demoTaintAnalysis(Module &module) {
  auto result = runTaintAnalysis(module);

  errs() << "[WPDS][Taint] Analysis Results:\n";
  for (auto &F : module) {
    if (F.isDeclaration())
      continue;

    for (auto &BB : F) {
      for (auto &I : BB) {
        const std::set<Value *> &in = result->IN(&I);

        // Check for sinks
        if (auto *CI = dyn_cast<CallBase>(&I)) {
          if (Function *F = CI->getCalledFunction()) {
            StringRef name = F->getName();
            // Check for dangerous sinks
            if (name.contains("system") || name.contains("exec") ||
                name.contains("strcpy") || name.contains("sprintf") ||
                name.contains("printf")) { // printf can be format string vuln

              bool taintedArg = false;
              for (auto &arg : CI->args()) {
                if (in.count(arg.get())) {
                  taintedArg = true;
                  break;
                }
              }

              if (taintedArg) {
                errs()
                    << "    [WARNING] Tainted data flows into dangerous sink: "
                    << name << " at instruction: ";
                I.print(errs());
                errs() << "\n";
              }
            }
          }
        }
      }
    }
  }
}
