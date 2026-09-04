//===- LeakChecker.cpp -- Memory leak detector ------------------------------//
//
// Migrated from SVF's SABER engine to Lotus.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/LeakChecker.h"

#include "Checker/Framework/BugReport.h"
#include "Checker/Framework/BugReportMgr.h"
#include "Checker/Framework/BugTypes.h"
#include "Checker/Saber/SaberCheckerAPI.h"
#include "Checker/Saber/SaberOptions.h"
#include "IR/ICFG/CallGraph.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <unordered_set>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace lotus::analysis;

static void appendPathConditionEvents(BugReport *report,
                                      const ProgSlice *slice) {
  if (!report || !slice)
    return;
  ProgSlice::EventStack events;
  slice->evalFinalCond2Event(events);
  for (const auto &e : events) {
    if (!e.first)
      continue;
    const std::vector<NodeTag> tags = {e.second ? NodeTag::CONDITION_TRUE
                                                : NodeTag::CONDITION_FALSE};
    report->append_step(const_cast<Instruction *>(e.first), "Path condition", 0,
                        tags);
  }
}

static const llvm::Function *getDirectCallee(const llvm::CallBase *call) {
  if (!call)
    return nullptr;
  if (const llvm::Function *callee = call->getCalledFunction())
    return callee;
  const llvm::Value *called = call->getCalledOperand();
  if (!called)
    return nullptr;
  return llvm::dyn_cast<llvm::Function>(called->stripPointerCasts());
}

static void appendUniqueCallee(std::vector<const llvm::Function *> &callees,
                               const llvm::Function *callee) {
  if (!callee)
    return;
  if (std::find(callees.begin(), callees.end(), callee) == callees.end())
    callees.push_back(callee);
}

static std::vector<const llvm::Function *>
collectResolvedCallees(const llvm::CallBase *call, const SVFG *graph,
                       SaberSVFGBuilder &builder) {
  std::vector<const llvm::Function *> callees;
  if (!call)
    return callees;
  if (const llvm::Function *directCallee = getDirectCallee(call)) {
    appendUniqueCallee(callees, directCallee);
    return callees;
  }

  if (graph) {
    for (const llvm::Function *callee : graph->getConnectedCallees(call))
      appendUniqueCallee(callees, callee);

    if (callees.empty()) {
      if (const LTCallGraph *callGraph = graph->getRefinedCallGraph()) {
        const llvm::Function *caller = call->getFunction();
        if (caller) {
          for (const auto &entry : *callGraph) {
            if (entry.first != caller || !entry.second)
              continue;
            for (const auto &record : *entry.second) {
              if (record.first != call || !record.second)
                continue;
              appendUniqueCallee(callees, record.second->getFunction());
            }
            break;
          }
        }
      }
    }
  }

  if (callees.empty()) {
    for (const llvm::Function *callee : builder.getIndirectCallTargets(call))
      appendUniqueCallee(callees, callee);
  }
  return callees;
}

static bool isUncalledFunction(const Function *fun) {
  if (!fun)
    return true;
  if (fun->hasAddressTaken())
    return false;
  if (fun->getName() == "main")
    return false;
  for (const User *user : fun->users()) {
    if (isa<CallBase>(user))
      return false;
  }
  return true;
}

static void collectBitcastForwardedPointerLoads(
    const llvm::Value *root,
    llvm::SmallVectorImpl<const llvm::LoadInst *> &loads) {
  if (!root || !root->getType()->isPointerTy())
    return;

  std::deque<const llvm::Value *> worklist;
  std::unordered_set<const llvm::Value *> visited;
  worklist.push_back(root);

  while (!worklist.empty()) {
    const llvm::Value *cur = worklist.front();
    worklist.pop_front();
    if (!visited.insert(cur).second)
      continue;

    for (const llvm::User *user : cur->users()) {
      if (const auto *load = dyn_cast<LoadInst>(user)) {
        if (load->getPointerOperand() == cur)
          loads.push_back(load);
        continue;
      }
      if (const auto *cast = dyn_cast<BitCastInst>(user)) {
        if (cast->getType()->isPointerTy())
          worklist.push_back(cast);
        continue;
      }
      if (const auto *cast = dyn_cast<AddrSpaceCastInst>(user)) {
        if (cast->getType()->isPointerTy())
          worklist.push_back(cast);
        continue;
      }
    }
  }
}

void LeakChecker::initSrcs() {
  if (!module_ || !svfg)
    return;

  CSWorkList worklist;
  SVFGNodeBS visited;

  for (auto &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallBase>(&I)) {
          if (isUncalledFunction(CI->getCaller()))
            continue;
          if (!CI->getType()->isPointerTy())
            continue;
          bool sourceLike = false;
          for (const llvm::Function *c :
               collectResolvedCallees(CI, svfg, memSSA)) {
            if (c && isSourceLikeFun(c->getName().str())) {
              sourceLike = true;
              break;
            }
          }
          if (sourceLike)
            worklist.push_back(CI);
        }
      }
    }
  }

  while (!worklist.empty()) {
    const llvm::CallBase *cs = worklist.front();
    worklist.pop_front();

    if (!cs->getCaller())
      continue;
    const llvm::Function *caller = cs->getCaller();
    if (!caller || caller->isDeclaration())
      continue;

    SVFGNode *node = svfg->getDef(cs);
    if (!node)
      node = svfg->getValueNode(cs);
    if (!node)
      continue;
    if (visited.count(node->getId()))
      continue;
    visited.insert(node->getId());

    CallSiteSet csSet;
    if (isInAWrapper(node, csSet)) {
      for (const llvm::CallBase *c : csSet)
        worklist.push_back(c);
    } else {
      if (!isUncalledFunction(caller) &&
          !SaberCheckerAPI::getCheckerAPI()->isExtCall(caller)) {
        addToSources(node);
        addSrcToCSID(node, cs);
      }
    }
  }
}

void LeakChecker::initSnks() {
  if (!module_ || !svfg)
    return;

  for (auto &F : *module_) {
    if (F.isDeclaration())
      continue;

    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallBase>(&I)) {
          bool sinkLike = false;
          bool multiLevelSink = false;
          for (const llvm::Function *c :
               collectResolvedCallees(CI, svfg, memSSA)) {
            if (!c)
              continue;
            if (isSinkLikeFun(c->getName().str())) {
              sinkLike = true;
              multiLevelSink =
                  multiLevelSink ||
                  SaberCheckerAPI::getCheckerAPI()->isMultiLevelMemDealloc(c);
            }
          }
          if (!sinkLike)
            continue;

          const auto &actualParms = svfg->getActualParms(CI);
          unsigned argIndex = 0;
          for (auto &arg : CI->args()) {
            if (!arg->getType()->isPointerTy()) {
              ++argIndex;
              continue;
            }

            SVFGNode *actualParmNode = nullptr;
            for (SVFGNode *n : actualParms) {
              if (!n)
                continue;
              if (n->getNodeKind() != SVFGK::ActualParm)
                continue;
              auto *ap = llvm::dyn_cast<ActualParmSVFGNode>(n);
              if (!ap)
                continue;
              if (ap->getParamIndex() == argIndex) {
                actualParmNode = n;
                break;
              }
            }

            if (actualParmNode)
              addToSinks(actualParmNode);

            SVFGNode *snkNode = svfg->getValueNode(arg.get());
            if (snkNode) {
              if (!actualParmNode)
                addToSinks(snkNode);
              if (multiLevelSink &&
                  arg->getType()->getPointerElementType()->isPointerTy()) {
                llvm::SmallVector<const LoadInst *, 4> forwardedLoads;
                collectBitcastForwardedPointerLoads(arg.get(), forwardedLoads);
                for (const LoadInst *load : forwardedLoads) {
                  if (SVFGNode *loadNode = svfg->getDef(load)) {
                    addToSinks(loadNode);
                  } else if (SVFGNode *loadNode = svfg->getValueNode(load)) {
                    addToSinks(loadNode);
                  }
                }
              }
            }

            ++argIndex;
          }
        }
      }
    }
  }
}

void LeakChecker::reportBug(ProgSlice *slice) {
  const SVFGNode *source = slice->getSource();
  if (!source)
    return;
  const llvm::CallBase *sourceCall = getSrcCSID(source);
  const llvm::Value *reportSource =
      sourceCall ? static_cast<const llvm::Value *>(sourceCall)
                 : static_cast<const llvm::Value *>(source->getInstruction());

  // Match SVF: only report when there is a leak (never free or partial leak).
  const bool allReachable = isAllPathReachable();
  const bool someReachable = isSomePathReachable();
  if (allReachable && someReachable)
    return; // No leak: all paths reach a free.

  BugReportMgr &mgr = BugReportMgr::get_instance();
  const bool neverFree = (!allReachable && !someReachable);
  int bugTypeId = mgr.register_bug_type(
      neverFree ? "Memory Leak" : "Memory Leak 2", BugDescription::BI_HIGH,
      BugDescription::BC_PERFORMANCE, "CWE-401");

  BugReport *report = new BugReport(bugTypeId);

  if (reportSource) {
    std::string tip = !someReachable
                          ? "Memory allocated here is never freed"
                          : "Memory may leak on some paths (partial leak)";
    report->append_step(const_cast<Value *>(reportSource), tip);
  }
  if (!neverFree)
    appendPathConditionEvents(report, slice);
  if (reportSource) {
    report->append_step(const_cast<Value *>(reportSource),
                        neverFree ? "Allocated memory is never freed"
                                  : "Allocated memory may leak on some paths");
  }

  mgr.insert_report(bugTypeId, report, false);

  if (SaberValidateTests)
    testsValidation(slice);

  outs() << "Memory Leak detected at ";
  if (const auto *inst = dyn_cast_or_null<Instruction>(reportSource)) {
    if (const Function *F = inst->getFunction()) {
      outs() << F->getName();
    }
  }
  outs() << "\n";
}

void LeakChecker::testsValidation(const ProgSlice *slice) {
  const SVFGNode *source = slice ? slice->getSource() : nullptr;
  if (!source)
    return;
  const llvm::CallBase *cs = getSrcCSID(source);
  if (!cs)
    return;
  const llvm::Function *fun = cs->getCalledFunction();
  if (!fun)
    return;

  const std::string funName = fun->getName().str();
  validateSuccessTests(source, funName);
  validateExpectedFailureTests(source, funName);
}

void LeakChecker::validateSuccessTests(const SVFGNode *source,
                                       const std::string &fun) {
  bool success = false;
  if (fun == "SAFEMALLOC") {
    success = (isAllPathReachable() && isSomePathReachable());
  } else if (fun == "NFRMALLOC") {
    success = (!isAllPathReachable() && !isSomePathReachable());
  } else if (fun == "PLKMALLOC") {
    success = (!isAllPathReachable() && isSomePathReachable());
  } else if (fun == "CLKMALLOC") {
    success = (!isAllPathReachable() && !isSomePathReachable());
  } else if (fun == "NFRLEAKFP" || fun == "PLKLEAKFP" || fun == "LEAKFN") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source && source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (success) {
    outs() << "\t SUCCESS :" << srcFun << " (src id:" << source->getId()
           << ")\n";
    return;
  }
  errs() << "\t FAILURE :" << srcFun << " (src id:" << source->getId() << ")\n";
  assert(false && "SABER leak validation failed");
}

void LeakChecker::validateExpectedFailureTests(const SVFGNode *source,
                                               const std::string &fun) {
  bool expectedFailure = false;
  if (fun == "NFRLEAKFP") {
    expectedFailure = (!isAllPathReachable() && !isSomePathReachable());
  } else if (fun == "PLKLEAKFP") {
    expectedFailure = (!isAllPathReachable() && isSomePathReachable());
  } else if (fun == "LEAKFN") {
    expectedFailure = (isAllPathReachable() && isSomePathReachable());
  } else if (fun == "SAFEMALLOC" || fun == "NFRMALLOC" || fun == "PLKMALLOC" ||
             fun == "CLKMALLOC") {
    return;
  } else {
    errs() << "SABER validation skipped: unknown test function " << fun << "\n";
    return;
  }

  const std::string srcFun = source && source->getFunction()
                                 ? source->getFunction()->getName().str()
                                 : std::string("<unknown>");
  if (expectedFailure) {
    outs() << "\t EXPECTED-FAILURE :" << srcFun
           << " (src id:" << source->getId() << ")\n";
    return;
  }
  errs() << "\t UNEXPECTED FAILURE :" << srcFun
         << " (src id:" << source->getId() << ")\n";
  assert(false && "SABER leak unexpected validation result");
}
