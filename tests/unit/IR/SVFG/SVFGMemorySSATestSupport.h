#pragma once

#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGNode.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/Instructions.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

class SVFGMemorySSATest : public LlvmModuleTest {
protected:
  std::unique_ptr<SVFG> buildSVFG(Module *module, ICFG &icfg) {
    ICFGBuilder icfgBuilder(&icfg);
    icfgBuilder.build(module);

    SVFGBuilderConfig cfg;
    cfg.usePointerAnalysis = false;
    cfg.buildMSSA = true;

    SVFGBuilder builder(cfg);
    return std::unique_ptr<SVFG>(builder.build(&icfg));
  }

  static const CallBase *findSingleIndirectCall(const Function *F) {
    for (const BasicBlock &BB : *F) {
      for (const Instruction &I : BB) {
        const auto *CB = dyn_cast<CallBase>(&I);
        if (CB && !CB->getCalledFunction())
          return CB;
      }
    }
    return nullptr;
  }

  static const LoadInst *findSingleLoad(const Function *F) {
    for (const BasicBlock &BB : *F)
      for (const Instruction &I : BB)
        if (const auto *LI = dyn_cast<LoadInst>(&I))
          return LI;
    return nullptr;
  }

  static bool callGraphHasEdge(const LTCallGraph &cg, const Function *caller,
                               const Instruction *callInst,
                               const Function *callee) {
    const LTCallGraphNode *node = cg[caller];
    for (const auto &record : *node) {
      if (record.first == callInst && record.second &&
          record.second->getFunction() == callee)
        return true;
    }
    return false;
  }
};
