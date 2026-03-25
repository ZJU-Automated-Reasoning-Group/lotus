//===- DDAClient.cpp -- DDA clients (SVF-style) ---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/DDAClient.h"

#include "Alias/DDA/ContextDDA.h"
#include "Alias/DDA/FlowDDA.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGNode.h"

#include <set>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

using namespace lotus::analysis;
using namespace llvm;

void DDAClient::addCandidate(const llvm::Value *v) {
  if (!v || !v->getType()->isPointerTy())
    return;
  if (!candidateQuerySet_.insert(v).second)
    return;
  candidateQueries_.push_back(v);
}

void DDAClient::resetCandidateQueries() {
  candidateQueries_.clear();
  candidateQuerySet_.clear();
}

std::vector<const llvm::Value *> &DDAClient::collectCandidateQueries() {
  resetCandidateQueries();
  if (!svfg_)
    return candidateQueries_;
  if (!solveAll_) {
    // User-specified query mode: trust explicit pointer list.
    for (const llvm::Value *v : userQueries_)
      addCandidate(v);
    return candidateQueries_;
  }
  // Match SVF's "all valid pointers" intent more closely by collecting every
  // pointer-typed value bound in the SVFG value index, including aliases that
  // share a canonical node (e.g. bitcast constant expressions). Fall back to
  // node scanning as a compatibility path for graphs without a populated value
  // index.
  for (const auto &entry : svfg_->getValueNodeMap()) {
    const llvm::Value *v = entry.first;
    if (!v || llvm::isa<llvm::ConstantPointerNull>(v))
      continue;
    addCandidate(v);
  }
  if (!candidateQueries_.empty())
    return candidateQueries_;

  for (auto it = svfg_->begin(), e = svfg_->end(); it != e; ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    const llvm::Value *v = node->getValue();
    if (!v || !v->getType()->isPointerTy() ||
        llvm::isa<llvm::ConstantPointerNull>(v))
      continue;
    if (node->isStmtNode() || node->isPhiNode() || node->isParamNode())
      addCandidate(v);
  }
  return candidateQueries_;
}

void DDAClient::answerQueries(FlowDDA *dda) {
  if (!dda || !dda->getSVFG())
    return;
  setSVFG(dda->getSVFG());
  collectCandidateQueries();
  // Force demand solving per candidate; results are cached in FlowDDA.
  for (const llvm::Value *ptr : candidateQueries_)
    (void)dda->getPointsTo(ptr);
  performStat(dda);
}

void DDAClient::answerQueries(ContextDDA *dda) {
  if (!dda || !dda->getSVFG())
    return;
  setSVFG(dda->getSVFG());
  if (FlowDDA *flow = dda->getFlowDDA()) {
    if (const llvm::Module *M = flow->getModule())
      setModule(M);
  }
  collectCandidateQueries();
  for (const llvm::Value *ptr : candidateQueries_)
    (void)dda->computeDDAPts(ptr);
  performStat(dda);
}

void DDAClient::performStat(ContextDDA *dda) {
  if (!dda)
    return;
  performStat(dda->getFlowDDA());
}

std::vector<const llvm::Value *> &FunptrDDAClient::collectCandidateQueries() {
  resetCandidateQueries();
  if (!solveAll_ && !userQueries_.empty()) {
    for (const llvm::Value *v : userQueries_)
      addCandidate(v);
    return candidateQueries_;
  }
  if (svfg_) {
    // Prefer SVFG's indirect-call map: it includes on-the-fly connected sites.
    for (const auto &pair : svfg_->getIndCallSiteMap()) {
      for (const llvm::CallBase *cb : pair.second) {
        if (!cb || cb->getCalledFunction())
          continue;
        const llvm::Value *called = cb->getCalledOperand();
        if (called && called->getType()->isPointerTy())
          addCandidate(called);
      }
    }
    if (!candidateQueries_.empty())
      return candidateQueries_;
  }
  if (!module_)
    return candidateQueries_;
  // Fallback when SVFG map is unavailable: scan IR for unresolved calls.
  for (const Function &F : *module_) {
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const CallBase *cb = llvm::dyn_cast<CallBase>(&I);
        if (!cb || cb->getCalledFunction())
          continue;
        const llvm::Value *called = cb->getCalledOperand();
        if (called && called->getType()->isPointerTy())
          addCandidate(called);
      }
    }
  }
  return candidateQueries_;
}

void FunptrDDAClient::performStat(FlowDDA *dda) {
  if (!dda || !module_)
    return;

  uint32_t totalCallsites = 0;
  uint32_t zeroTargetCallsites = 0;
  uint32_t oneTargetCallsites = 0;
  uint32_t twoTargetCallsites = 0;
  uint32_t moreThanTwoCallsites = 0;

  for (const Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        const CallBase *cb = dyn_cast<CallBase>(&I);
        if (!cb || cb->getCalledFunction())
          continue;
        const Value *called = cb->getCalledOperand();
        if (!called || !called->getType()->isPointerTy())
          continue;

        ++totalCallsites;
        std::unordered_set<const Function *> targetFuncs;
        const auto pts = dda->getPointsTo(called);
        for (uint32_t objId : pts) {
          if (const Value *v = dda->getSVFGConst()
                                   ? dda->getSVFGConst()->getObjectValue(objId)
                                   : nullptr) {
            if (const auto *callee = dyn_cast<Function>(v))
              targetFuncs.insert(callee);
          }
        }

        if (targetFuncs.empty())
          ++zeroTargetCallsites;
        else if (targetFuncs.size() == 1)
          ++oneTargetCallsites;
        else if (targetFuncs.size() == 2)
          ++twoTargetCallsites;
        else
          ++moreThanTwoCallsites;
      }
    }
  }

  llvm::outs() << "=== FunptrDDAClient Stats ===\n";
  llvm::outs() << "Indirect callsites: " << totalCallsites << "\n";
  llvm::outs() << "  zero-target: " << zeroTargetCallsites << "\n";
  llvm::outs() << "  one-target: " << oneTargetCallsites << "\n";
  llvm::outs() << "  two-target: " << twoTargetCallsites << "\n";
  llvm::outs() << "  >2-target: " << moreThanTwoCallsites << "\n";
}

std::vector<const llvm::Value *> &AliasDDAClient::collectCandidateQueries() {
  resetCandidateQueries();
  if (!solveAll_ && !userQueries_.empty()) {
    for (const llvm::Value *v : userQueries_)
      addCandidate(v);
    return candidateQueries_;
  }
  if (!svfg_)
    return candidateQueries_;
  std::unordered_set<const llvm::Value *> seen;
  // Alias-focused query set: pointer operands used by load/store/GEP.
  for (auto it = svfg_->begin(), e = svfg_->end(); it != e; ++it) {
    SVFGNode *node = it->second;
    if (!node)
      continue;
    const llvm::Value *ptr = nullptr;
    if (node->getNodeKind() == SVFGK::Load) {
      const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(node);
      if (llvm::isa_and_nonnull<LoadInst>(load->getValue()))
        ptr = llvm::cast<LoadInst>(load->getValue())->getPointerOperand();
    } else if (node->getNodeKind() == SVFGK::Store) {
      const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(node);
      if (llvm::isa_and_nonnull<StoreInst>(store->getValue()))
        ptr = llvm::cast<StoreInst>(store->getValue())->getPointerOperand();
    } else if (node->getNodeKind() == SVFGK::Gep) {
      const GepSVFGNode *gep = llvm::cast<GepSVFGNode>(node);
      if (llvm::isa_and_nonnull<GetElementPtrInst>(gep->getValue()))
        ptr =
            llvm::cast<GetElementPtrInst>(gep->getValue())->getPointerOperand();
    }
    if (ptr && ptr->getType()->isPointerTy() && seen.insert(ptr).second)
      addCandidate(ptr);
  }
  return candidateQueries_;
}

void AliasDDAClient::performStat(FlowDDA *dda) {
  if (!dda || !module_)
    return;

  std::vector<const Value *> loadPtrs;
  std::vector<const Value *> storePtrs;
  std::set<std::pair<const Value *, const Value *>> seenPairs;

  for (const Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const BasicBlock &BB : F) {
      for (const Instruction &I : BB) {
        if (const auto *LI = dyn_cast<LoadInst>(&I)) {
          const Value *ptr = LI->getPointerOperand();
          if (ptr && ptr->getType()->isPointerTy())
            loadPtrs.push_back(ptr);
        } else if (const auto *SI = dyn_cast<StoreInst>(&I)) {
          const Value *ptr = SI->getPointerOperand();
          if (ptr && ptr->getType()->isPointerTy())
            storePtrs.push_back(ptr);
        }
      }
    }
  }

  uint32_t totalPairs = 0;
  uint32_t mayAliasPairs = 0;
  for (const Value *lp : loadPtrs) {
    for (const Value *sp : storePtrs) {
      if (!seenPairs.insert(std::make_pair(lp, sp)).second)
        continue;
      ++totalPairs;
      if (dda->mayAlias(lp, sp))
        ++mayAliasPairs;
    }
  }

  llvm::outs() << "=== AliasDDAClient Stats ===\n";
  llvm::outs() << "Load/Store alias pairs: " << totalPairs << "\n";
  llvm::outs() << "  may-alias pairs: " << mayAliasPairs << "\n";
}
