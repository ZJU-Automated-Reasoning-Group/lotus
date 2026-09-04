#pragma once

#include "Checker/Saber/SaberCondAllocator.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Saber/DoubleFreeChecker.h"
#include "Checker/Saber/FileChecker.h"
#include "Checker/Saber/LeakChecker.h"
#include "Checker/Saber/SaberOptions.h"
#include "Checker/Saber/SaberSVFGBuilder.h"
#include "Checker/Saber/SrcSnkDDA.h"
#include "IR/ICFG/ICFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using lotus::unittest::parseModule;

namespace {

const BasicBlock *getBlock(const Module &module, StringRef functionName,
                           StringRef blockName) {
  const Function *function = module.getFunction(functionName);
  if (!function)
    return nullptr;
  for (const BasicBlock &bb : *function) {
    if (bb.getName() == blockName)
      return &bb;
  }
  return nullptr;
}

const CallBase *getOnlyCall(const Module &module, StringRef functionName) {
  const Function *function = module.getFunction(functionName);
  if (!function)
    return nullptr;
  const CallBase *call = nullptr;
  for (const BasicBlock &bb : *function) {
    for (const Instruction &inst : bb) {
      if (const auto *cb = dyn_cast<CallBase>(&inst)) {
        if (call)
          return nullptr;
        call = cb;
      }
    }
  }
  return call;
}

const llvm::Function *getResolvedCallee(const CallBase *call) {
  if (!call)
    return nullptr;
  if (const auto *callee = call->getCalledFunction())
    return callee;
  const Value *called = call->getCalledOperand();
  if (!called)
    return nullptr;
  return dyn_cast<Function>(called->stripPointerCasts());
}

std::vector<const CallBase *> getCallsTo(const Module &module,
                                         StringRef functionName,
                                         StringRef calleeName) {
  std::vector<const CallBase *> calls;
  const Function *function = module.getFunction(functionName);
  if (!function)
    return calls;
  for (const BasicBlock &bb : *function) {
    for (const Instruction &inst : bb) {
      const auto *cb = dyn_cast<CallBase>(&inst);
      if (!cb)
        continue;
      const Function *callee = getResolvedCallee(cb);
      if (callee && callee->getName() == calleeName)
        calls.push_back(cb);
    }
  }
  return calls;
}

const StoreInst *getOnlyStore(const Module &module, StringRef functionName) {
  const Function *function = module.getFunction(functionName);
  if (!function)
    return nullptr;
  const StoreInst *store = nullptr;
  for (const BasicBlock &bb : *function) {
    for (const Instruction &inst : bb) {
      if (const auto *si = dyn_cast<StoreInst>(&inst)) {
        if (store)
          return nullptr;
        store = si;
      }
    }
  }
  return store;
}

size_t getReportCountForType(BugReportMgr &mgr, StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0)
    return 0;
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  return reports ? reports->size() : 0;
}

size_t getMemoryLeakReportCount(BugReportMgr &mgr) {
  return getReportCountForType(mgr, "Memory Leak") +
         getReportCountForType(mgr, "Memory Leak 2");
}

const BugReport *getLastReportForType(BugReportMgr &mgr, StringRef bugTypeName) {
  int bugTypeId = mgr.find_bug_type(bugTypeName);
  if (bugTypeId < 0)
    return nullptr;
  const auto *reports = mgr.get_reports_for_type(bugTypeId);
  if (!reports || reports->empty())
    return nullptr;
  return reports->back();
}

bool reportHasStepTip(const BugReport *report, StringRef tip) {
  if (!report)
    return false;
  for (const BugDiagStep *step : report->get_steps()) {
    if (step && step->tip == tip)
      return true;
  }
  return false;
}

} // namespace

namespace {

class DummySrcSnkDDA final : public SrcSnkDDA {
public:
  void initSrcs() override {}
  void initSnks() override {}
  bool isSourceLikeFun(const std::string &) override { return false; }
  bool isSinkLikeFun(const std::string &) override { return false; }
  void reportBug(ProgSlice *) override {}
};

class TestSaberSVFGBuilder final : public SaberSVFGBuilder {
public:
  bool isStrongUpdatePublic(const SVFGNode *node, uint32_t &singleton) {
    return isStrongUpdate(node, singleton);
  }
};

class SaberOptionScope {
public:
  SaberOptionScope()
      : oldFullSVFG_(SaberFullSVFG), oldCxtLimit_(SaberCxtLimit) {}

  ~SaberOptionScope() {
    SaberFullSVFG = oldFullSVFG_;
    SaberCxtLimit = oldCxtLimit_;
  }

private:
  bool oldFullSVFG_;
  unsigned oldCxtLimit_;
};

} // namespace
