//===- ProgSlice.cpp -- Program slicing based on SVF-----------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// AllPathReachableSolve and path-condition logic aligned with SVF.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/ProgSlice.h"

#include "Checker/Saber/SaberCondAllocator.h"
#include "IR/ICFG/ICFGNode.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instructions.h>

using namespace llvm;
using namespace lotus::analysis;

bool ProgSlice::AllPathReachableSolve() {
  const SVFGNode *source = getSource();
  VFWorkList worklist;
  worklist.push_back(source);
  setVFCond(source, pathAllocator->getTrueCond());

  while (!worklist.empty()) {
    const SVFGNode *node = worklist.front();
    worklist.pop_front();
    setCurSVFGNode(node);

    Condition invalidCond = computeInvalidCondFromRemovedSUVFEdge(node);
    Condition cond = getVFCond(node);

    for (SVFGEdge *edge : node->getOutEdges()) {
      const SVFGNode *succ = edge->getDstNode();
      if (!inBackwardSlice(succ))
        continue;

      const BasicBlock *nodeBB = getSVFGNodeBB(node);
      const BasicBlock *succBB = getSVFGNodeBB(succ);
      pathAllocator->clearCFCond();

      Condition vfCond;
      if (edge->isCallEdge()) {
        const BasicBlock *callBB =
            edge->getCallSite() ? edge->getCallSite()->getParent() : nullptr;
        vfCond =
            pathAllocator->ComputeInterCallVFGGuard(nodeBB, succBB, callBB);
      } else if (edge->isRetEdge()) {
        const BasicBlock *retBB =
            edge->getCallSite() ? edge->getCallSite()->getParent() : nullptr;
        vfCond = pathAllocator->ComputeInterRetVFGGuard(nodeBB, succBB, retBB);
      } else {
        vfCond = pathAllocator->ComputeIntraVFGGuard(nodeBB, succBB);
      }

      vfCond =
          pathAllocator->condAnd(vfCond, pathAllocator->condNeg(invalidCond));
      Condition succPathCond = pathAllocator->condAnd(cond, vfCond);
      Condition newSuccCond =
          pathAllocator->condOr(getVFCond(succ), succPathCond);

      if (setVFCond(succ, newSuccCond))
        worklist.push_back(succ);
    }
  }

  return isSatisfiableForAll();
}

bool ProgSlice::isSatisfiableForAll() {
  Condition guard = pathAllocator->getFalseCond();
  for (auto it = sinksBegin(), eit = sinksEnd(); it != eit; ++it)
    guard = pathAllocator->condOr(guard, getVFCond(*it));
  setFinalCond(guard);
  return pathAllocator->isAllPathReachable(guard);
}

bool ProgSlice::isSatisfiableForPairs() {
  for (auto it = sinksBegin(), eit = sinksEnd(); it != eit; ++it) {
    for (auto sit = it; sit != eit; ++sit) {
      if (*it == *sit)
        continue;
      Condition guard = pathAllocator->condAnd(getVFCond(*sit), getVFCond(*it));
      if (!pathAllocator->isEquivalentBranchCond(
              guard, pathAllocator->getFalseCond())) {
        setFinalCond(guard);
        return false;
      }
    }
  }
  return true;
}

const BasicBlock *ProgSlice::getSVFGNodeBB(const SVFGNode *node) const {
  if (!node)
    return nullptr;
  const ICFGNode *icfgNode = node->getICFGNode();
  if (icfgNode)
    return icfgNode->getBasicBlock();
  const Instruction *inst = node->getInstruction();
  return inst ? inst->getParent() : nullptr;
}

std::string ProgSlice::evalFinalCond() const {
  std::string result;
  std::set<std::string> locations;
  SaberCondAllocator::NodeBS support;
  pathAllocator->extractSubConds(finalCond, support);

  for (uint32_t id : support) {
    const llvm::Instruction *inst = pathAllocator->getCondInst(id);
    if (!inst)
      continue;
    std::string locStr;
    if (const llvm::DebugLoc &DL = inst->getDebugLoc()) {
      llvm::raw_string_ostream rso(locStr);
      DL.print(rso);
    } else if (inst->getParent()) {
      locStr = inst->getParent()->getParent()->getName().str() + ":0";
    }
    if (pathAllocator->isNegCond(id))
      locations.insert(locStr + "|False");
    else
      locations.insert(locStr + "|True");
  }

  for (const auto &loc : locations) {
    result += "\t\t  --> (" + loc + ") \n";
  }

  return result;
}

void ProgSlice::evalFinalCond2Event(EventStack &eventStack) const {
  SaberCondAllocator::NodeBS support;
  pathAllocator->extractSubConds(finalCond, support);
  for (uint32_t id : support) {
    const llvm::Instruction *inst = pathAllocator->getCondInst(id);
    if (inst) {
      bool branchTaken = !pathAllocator->isNegCond(id);
      eventStack.emplace_back(inst, branchTaken);
    }
  }
}

const llvm::CallBase *ProgSlice::getCallSite(const SVFGEdge *edge) const {
  if (!edge)
    return nullptr;
  if (edge->isCallEdge())
    return edge->getCallSite();
  return nullptr;
}

const llvm::CallBase *ProgSlice::getRetSite(const SVFGEdge *edge) const {
  if (!edge)
    return nullptr;
  if (edge->isRetEdge())
    return edge->getCallSite();
  return nullptr;
}

ProgSlice::Condition
ProgSlice::computeInvalidCondFromRemovedSUVFEdge(const SVFGNode *cur) {
  std::set<const llvm::BasicBlock *> validOutBBs;
  for (SVFGEdge *edge : cur->getOutEdges()) {
    const SVFGNode *succ = edge->getDstNode();
    if (inBackwardSlice(succ)) {
      const llvm::BasicBlock *succBB = getSVFGNodeBB(succ);
      if (succBB)
        validOutBBs.insert(succBB);
    }
  }

  Condition invalidCond = pathAllocator->getFalseCond();
  auto it = getRemovedSUVFEdges().find(cur);
  if (it != getRemovedSUVFEdges().end()) {
    for (const SVFGNode *succ : it->second) {
      const llvm::BasicBlock *succBB = getSVFGNodeBB(succ);
      if (!succBB || validOutBBs.count(succBB))
        continue;
      const llvm::BasicBlock *nodeBB = getSVFGNodeBB(cur);
      if (!nodeBB)
        continue;
      pathAllocator->clearCFCond();
      invalidCond = pathAllocator->condOr(
          invalidCond, pathAllocator->ComputeIntraVFGGuard(nodeBB, succBB));
    }
  }
  return invalidCond;
}
