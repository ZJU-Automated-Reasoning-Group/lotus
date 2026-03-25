//===- ContextDDA.cpp -- Context-sensitive DDA (SVF-style) ----------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/ContextDDA.h"

#include "Alias/DDA/DPItem.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"
#include "IR/SVFG/SVFGStats.h"

#include <algorithm>
#include <functional>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

using namespace lotus::analysis;
using namespace llvm;

namespace {

static bool isIndirectEdgeImpl(SVFGEdge *e);

static bool isDirectEdgeImpl(SVFGEdge *e) {
  if (!e)
    return false;
  if (isIndirectEdgeImpl(e))
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  if (isDirectVFGEdge(k))
    return true;
  return k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

static bool isIndirectEdgeImpl(SVFGEdge *e) {
  if (!e)
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  if (isIndirectVFGEdge(k)) {
    return true;
  }
  if (k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraCopy ||
      k == SVFGEdgeK::IntraDirect) {
    const SVFGNode *src = e->getSrcNode();
    const SVFGNode *dst = e->getDstNode();
    return (src && src->isMemNode()) || (dst && dst->isMemNode());
  }
  return false;
}

static bool isCallEdge(SVFGEdge *e) {
  if (!e)
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::CallDir || k == SVFGEdgeK::CallInd ||
         k == SVFGEdgeK::CallAIn || k == SVFGEdgeK::CallFIn ||
         k == SVFGEdgeK::ParamCall;
}

static bool isRetEdge(SVFGEdge *e) {
  if (!e)
    return false;
  SVFGEdgeK k = e->getEdgeKind();
  return k == SVFGEdgeK::RetDir || k == SVFGEdgeK::RetInd ||
         k == SVFGEdgeK::RetAOut || k == SVFGEdgeK::RetFOut ||
         k == SVFGEdgeK::ParamRet;
}

static std::vector<const llvm::Value *>
collectFallbackPointerValues(const CxtLocDPItem &dpm, SVFG *svfg) {
  // Same conservative value collection policy as FlowDDA fallback.
  std::vector<const llvm::Value *> values;
  if (const SVFGNode *loc = dpm.getLoc()) {
    if (const llvm::Value *locVal = loc->getValue()) {
      if (locVal->getType()->isPointerTy())
        values.push_back(locVal);
    }
  }
  if (svfg) {
    if (const llvm::Value *objVal = svfg->getObjectValue(dpm.getCurNodeID())) {
      if (objVal->getType()->isPointerTy() &&
          std::find(values.begin(), values.end(), objVal) == values.end()) {
        values.push_back(objVal);
      }
    }
  }
  return values;
}

} // namespace

bool ContextDDA::isDirectEdge(SVFGEdge *e) { return isDirectEdgeImpl(e); }
bool ContextDDA::isIndirectEdge(SVFGEdge *e) { return isIndirectEdgeImpl(e); }

ContextDDA::ContextDDA(FlowDDA *flowDDA, DDAClient *client)
    : flowDDA_(flowDDA), client_(client) {
  setDDAStat(flowDDA_ ? flowDDA_->getStat() : nullptr);
}

ContextDDA::~ContextDDA() = default;

uint32_t ContextDDA::getTopLevelValueId(const SVFGNode *node) const {
  if (!node)
    return 0;
  if (node->hasValueId())
    return node->getValueId();
  return node->getId();
}

bool ContextDDA::run(llvm::Module &M) {
  if (!flowDDA_ || !flowDDA_->run(M))
    return false;
  buildRecursionInfo();
  // Upstream SVF only weakens context sensitivity on recursion/value-flow
  // cycles when explicitly requested. Keep the default fully context-sensitive.
  if (insensitiveRecursion_ || insensitiveCycle_)
    initInsensitiveEdges();
  return true;
}

uint32_t ContextDDA::getCSIDAtCall(CxtLocDPItem &, SVFGEdge *edge) {
  SVFG *svfg = getSVFG();
  if (!svfg || !edge)
    return 0;
  const llvm::CallBase *cs = edge->getCallSite();
  if (!cs)
    return 0;
  const llvm::Function *callee =
      edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
  return svfg->getCallSiteId(cs, callee);
}

uint32_t ContextDDA::getCSIDAtRet(CxtLocDPItem &, SVFGEdge *edge) {
  SVFG *svfg = getSVFG();
  if (!svfg || !edge)
    return 0;
  const llvm::CallBase *cs = edge->getCallSite();
  if (!cs)
    return 0;
  const llvm::Function *callee =
      edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
  return svfg->getCallSiteId(cs, callee);
}

bool ContextDDA::isEdgeInRecursion(uint32_t csId) const {
  return recursiveCallSiteIds_.count(csId) != 0;
}

void ContextDDA::popRecursiveCallSites(CxtLocDPItem &dpm) {
  // SVF: mark context as non-concrete since we lose precision crossing
  // recursion.
  dpm.getCond().setNonConcreteCxt();
  while (!dpm.getCond().getContexts().empty() &&
         isEdgeInRecursion(dpm.getCond().getContexts().back()))
    dpm.getCond().popBack();
}

bool ContextDDA::handleBKCondition(CxtLocDPItem &dpm, SVFGEdge *edge) {
  if (client_)
    client_->handleStatement(edge->getSrcNode(), dpm.getCurNodeID());
  // Context-insensitive edge: skip context check (recursion or value-flow
  // cycle).
  if (insensitveEdges_.count(edge))
    return true;
  SVFG *svfg = getSVFG();
  if (!svfg)
    return true;

  if (isCallEdge(edge)) {
    uint32_t csId = getCSIDAtCall(dpm, edge);
    if (csId != 0) {
      if (isEdgeInRecursion(csId)) {
        // SVF: in recursion, just pop recursive call sites and skip
        // matchContext.
        popRecursiveCallSites(dpm);
      } else {
        // Not in recursion: match call string. If mismatch, prune this path.
        if (!dpm.matchContext(csId))
          return false;
      }
    }
  } else if (isRetEdge(edge)) {
    uint32_t csId = getCSIDAtRet(dpm, edge);
    if (csId != 0) {
      if (isEdgeInRecursion(csId)) {
        // SVF: in recursion, just pop recursive call sites and skip
        // pushContext.
        popRecursiveCallSites(dpm);
      } else {
        // SVF: if this call site ID is already in the call string, it may
        // indicate an undetected recursion. Mark as out-of-budget.
        if (dpm.getCond().containCallStr(csId)) {
          outOfBudget_ = true;
          return false;
        }
        // Push context for return edge (going backward = entering callee).
        dpm.pushContext(csId);
      }
    }
  }
  return true;
}

void ContextDDA::handleOutOfBudgetDpm(const CxtLocDPItem &dpm) {
  if (!flowDDA_ || !getSVFG())
    return;

  CxtPtSet downgradedPts;
  const std::vector<const llvm::Value *> values =
      collectFallbackPointerValues(dpm, getSVFG());
  for (const llvm::Value *v : values) {
    FlowDDA::PtsSet flowPts = flowDDA_->getPointsTo(v);
    for (uint32_t objId : flowPts)
      downgradedPts.insert(CxtVar(ContextCond(), objId));
  }

  if (downgradedPts.empty())
    downgradedPts = getConservativeCPts(dpm);

  if (!downgradedPts.empty())
    DDAVFSolver<CxtVar, CxtPtSet, CxtLocDPItem,
                ContextDDA>::updateCachedPointsTo(dpm, downgradedPts);
  DDAVFSolver<CxtVar, CxtPtSet, CxtLocDPItem, ContextDDA>::addOutOfBudgetDpm(
      dpm);
}

void ContextDDA::buildRecursionInfo() {
  recursiveCallSiteIds_.clear();
  const llvm::Module *module = flowDDA_ ? flowDDA_->getModule() : nullptr;
  SVFG *svfg = getSVFG();
  if (!module)
    return;
  auto appendUniqueCallees =
      [](std::vector<const llvm::Function *> &dst,
         const std::vector<const llvm::Function *> &src) {
        for (const llvm::Function *callee : src) {
          if (!callee)
            continue;
          if (std::find(dst.begin(), dst.end(), callee) == dst.end())
            dst.push_back(callee);
        }
      };
  std::unordered_map<const llvm::Function *,
                     std::vector<const llvm::Function *>>
      callGraph;
  for (const llvm::Function &F : *module) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration())
            callees.push_back(direct);
        } else if (svfg) {
          const auto &connected = svfg->getConnectedCallees(cb);
          for (const llvm::Function *callee : connected)
            callees.push_back(callee);
          if (flowDDA_ && flowDDA_->getSVFGBuilder())
            appendUniqueCallees(callees,
                                flowDDA_->getSVFGBuilder()->getIndirectCallTargets(cb));
        }
        for (const llvm::Function *callee : callees) {
          if (!callee || callee->isDeclaration())
            continue;
          callGraph[&F].push_back(callee);
        }
      }
  }
  // Bug 8 fix: replace recursive std::function Tarjan with iterative version
  // to avoid stack overflow on programs with deep call chains.
  std::unordered_map<const llvm::Function *, uint32_t> index, lowlink;
  std::stack<const llvm::Function *> stk;
  std::unordered_set<const llvm::Function *> onStack;
  uint32_t nextIndex = 0;
  std::vector<std::set<const llvm::Function *>> sccs;

  struct Frame {
    const llvm::Function *f;
    std::vector<const llvm::Function *>::const_iterator it;
    std::vector<const llvm::Function *>::const_iterator end;
  };
  std::stack<Frame> worklist;

  auto visit = [&](const llvm::Function *root) {
    if (index.count(root))
      return;
    worklist.push({root, {}, {}});
    while (!worklist.empty()) {
      Frame &frame = worklist.top();
      const llvm::Function *f = frame.f;
      if (!index.count(f)) {
        index[f] = lowlink[f] = nextIndex++;
        stk.push(f);
        onStack.insert(f);
        auto cgIt = callGraph.find(f);
        if (cgIt != callGraph.end()) {
          frame.it = cgIt->second.begin();
          frame.end = cgIt->second.end();
        } else {
          frame.it = frame.end = {};
        }
      }
      bool pushed = false;
      while (frame.it != frame.end) {
        const llvm::Function *callee = *frame.it;
        ++frame.it;
        if (!index.count(callee)) {
          worklist.push({callee, {}, {}});
          pushed = true;
          break;
        } else if (onStack.count(callee)) {
          lowlink[f] = std::min(lowlink[f], index[callee]);
        }
      }
      if (pushed)
        continue;
      worklist.pop();
      if (!worklist.empty()) {
        const llvm::Function *parent = worklist.top().f;
        lowlink[parent] = std::min(lowlink[parent], lowlink[f]);
      }
      if (lowlink[f] == index[f]) {
        std::set<const llvm::Function *> scc;
        const llvm::Function *w;
        do {
          w = stk.top();
          stk.pop();
          onStack.erase(w);
          scc.insert(w);
        } while (w != f);
        sccs.push_back(std::move(scc));
      }
    }
  };

  for (const llvm::Function &F : *module) {
    if (F.isDeclaration())
      continue;
    visit(&F);
  }

  std::unordered_map<const llvm::Function *, size_t> funcToScc;
  for (size_t i = 0; i < sccs.size(); ++i)
    for (const llvm::Function *f : sccs[i])
      funcToScc[f] = i;

  // Collect recursive SCCs: multi-function SCCs + self-recursive functions
  std::unordered_set<size_t> recursiveSccIds;
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (sccs[i].size() > 1) {
      recursiveSccIds.insert(i);
    } else if (sccs[i].size() == 1) {
      // Check for self-recursion (self-edge in call graph)
      const llvm::Function *f = *sccs[i].begin();
      auto it = callGraph.find(f);
      if (it != callGraph.end()) {
        for (const llvm::Function *callee : it->second) {
          if (callee == f) {
            recursiveSccIds.insert(i);
            break;
          }
        }
      }
    }
  }

  for (const llvm::Function &F : *module) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration())
            callees.push_back(direct);
        } else if (svfg) {
          const auto &connected = svfg->getConnectedCallees(cb);
          for (const llvm::Function *callee : connected)
            callees.push_back(callee);
        }
        for (const llvm::Function *callee : callees) {
          if (!callee || callee->isDeclaration())
            continue;
          auto itCaller = funcToScc.find(&F);
          auto itCallee = funcToScc.find(callee);
          if (itCaller == funcToScc.end() || itCallee == funcToScc.end())
            continue;
          // Both caller and callee must be in the same recursive SCC
          if (itCaller->second != itCallee->second)
            continue;
          if (recursiveSccIds.count(itCaller->second) == 0)
            continue;
          uint32_t csId = svfg ? svfg->getCallSiteId(cb, callee) : 0;
          if (csId != 0)
            recursiveCallSiteIds_.insert(csId);
        }
      }
  }
}

void ContextDDA::resetQueryLoadMaps() {
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
}

CxtPtSet ContextDDA::getConservativeCPts(const CxtLocDPItem &dpm) const {
  // Match SVF ContextDDA::getConservativeCPts: downgrade to FlowDDA
  if (!flowDDA_ || !getSVFG())
    return CxtPtSet{};

  std::vector<const llvm::Value *> values =
      collectFallbackPointerValues(dpm, getSVFG());
  if (values.empty())
    return CxtPtSet{};

  CxtPtSet cxtPts;
  ContextCond cond;
  for (const llvm::Value *v : values) {
    // Use AserPTA-backed object IDs through FlowDDA helper.
    SVFGNodeBS objIds = flowDDA_->getObjectIdsForValue(v);
    for (uint32_t objId : objIds) {
      CxtVar var(cond, objId);
      cxtPts.insert(var);
    }
  }
  if (cxtPts.empty() && flowDDA_->getSVFGBuilder()) {
    uint32_t unknown = flowDDA_->getSVFGBuilder()->getUnknownObjId();
    if (unknown != 0)
      cxtPts.insert(CxtVar(cond, unknown));
  }
  return cxtPts;
}

void ContextDDA::setDpmLocVar(CxtLocDPItem &dpm, SVFGNode *src,
                              uint32_t ptrNodeId) {
  dpm.setLocVar(src, ptrNodeId);
}

void ContextDDA::addDDAPts(CxtPtSet &pts, uint32_t var) {
  pts.insert(CxtVar(ContextCond(), var));
}

void ContextDDA::unionDDAPts(CxtPtSet &target, const CxtPtSet &source) {
  for (const CxtVar &v : source)
    target.insert(v);
}

bool ContextDDA::unionDDAPts(const CxtLocDPItem &dpm, const CxtPtSet &pts) {
  const SVFGNode *loc = dpm.getLoc();
  if (!loc)
    return false;
  auto &cache = isTopLevelPtrStmt(loc) ? dpmToTLPtsMap_ : dpmToADPtsMap_;
  auto it = cache.find(dpm);
  const size_t oldSize = (it != cache.end()) ? it->second.size() : 0;
  if (it == cache.end())
    cache[dpm] = pts;
  else
    for (const CxtVar &v : pts)
      it->second.insert(v);
  it = cache.find(dpm);
  return (it != cache.end() && it->second.size() > oldSize);
}

const CxtPtSet &ContextDDA::getEmptyCPtSetRef() const {
  static const CxtPtSet empty;
  return empty;
}

bool ContextDDA::isTopLevelPtrStmt(const SVFGNode *stmt) const {
  if (!stmt)
    return false;
  // Match SVF DDAVFSolver default: top-level unless Store or memory node.
  return stmt->getNodeKind() != SVFGK::Store && !stmt->isMemNode();
}

void ContextDDA::connectIndirectCallees(const CxtLocDPItem &dpm,
                                        const CxtPtSet &funPts,
                                        std::vector<SVFGEdge *> &newEdges) {
  SVFG *svfg = getSVFG();
  if (!flowDDA_ || !flowDDA_->getSVFGBuilder() || !svfg)
    return;
  const auto &indCallSites = svfg->getIndCallSites(dpm.getCurNodeID());
  for (const llvm::CallBase *cs : indCallSites) {
    if (!cs)
      continue;
    for (const CxtVar &cv : funPts) {
      uint32_t objId = cv.get_id();
      if (objId == 0)
        continue;
      const llvm::Value *v = svfg->getObjectValue(objId);
      const llvm::Function *callee = llvm::dyn_cast_or_null<llvm::Function>(v);
      if (!callee || callee->isDeclaration())
        continue;
      (void)flowDDA_->getSVFGBuilder()->connectCallSiteToCalleeOnTheFly(
          svfg, cs, callee, newEdges);
    }
  }
}

void ContextDDA::insertOutOfBudgetDpm(const CxtLocDPItem &dpm) {
  outOfBudgetDpms_.insert(dpm);
}

bool ContextDDA::isOutOfBudgetDpm(const CxtLocDPItem &dpm) const {
  return outOfBudgetDpms_.count(dpm) != 0;
}

void ContextDDA::forEachObjId(const CxtPtSet &pts,
                              std::function<void(uint32_t)> callback) const {
  for (const CxtVar &v : pts)
    callback(v.get_id());
}

void ContextDDA::forEachElementInCPtSet(
    const CxtPtSet &pts,
    std::function<void(const CxtVar &, uint32_t)> callback) const {
  for (const CxtVar &v : pts)
    callback(v, v.get_id());
}

SVFGNodeBS ContextDDA::getObjectIdsForValue(const llvm::Value *v) const {
  return flowDDA_ ? flowDDA_->getObjectIdsForValue(v) : SVFGNodeBS{};
}

void ContextDDA::addLoadDpmAndCVar(const CxtLocDPItem &dpm,
                                   const CxtLocDPItem &loadDpm,
                                   const CxtVar &loadCVar) {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    it->second = loadDpm;
  else
    dpmToLoadDpmMap_.emplace(dpm, loadDpm);
  dpmToLoadCVarMap_[dpm] = loadCVar;
}

bool ContextDDA::hasLoadDpm(const CxtLocDPItem &dpm) const {
  return dpmToLoadDpmMap_.find(dpm) != dpmToLoadDpmMap_.end();
}

CxtLocDPItem ContextDDA::getLoadDpm(const CxtLocDPItem &dpm) const {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    return it->second;
  // Callers must guard with hasLoadDpm() before calling this. Reaching here
  // means the load/store DPM linkage was never established, which indicates a
  // logic error in getDPImWithOldCond or the Store handler. Assert in debug
  // builds; in release, return the dpm itself so the solver can continue
  // (conservative: treats the store as a self-alias, which is sound but
  // imprecise) and emit a diagnostic so the bug is visible.
  assert(false && "ContextDDA::getLoadDpm: loadDpm not found; "
                  "caller should have checked hasLoadDpm() first");
  llvm::errs() << "[DDA bug] ContextDDA::getLoadDpm: loadDpm not found for dpm "
                  "(cur="
               << dpm.getCurNodeID()
               << "). "
                  "Returning self — points-to result may be unsound.\n";
  return dpm;
}

CxtVar ContextDDA::getLoadCVar(const CxtLocDPItem &dpm) const {
  auto it = dpmToLoadCVarMap_.find(dpm);
  if (it != dpmToLoadCVarMap_.end())
    return it->second;
  // Same contract as getLoadDpm: callers must check hasLoadDpm() first.
  assert(false && "ContextDDA::getLoadCVar: loadCVar not found; "
                  "caller should have checked hasLoadDpm() first");
  llvm::errs()
      << "[DDA bug] ContextDDA::getLoadCVar: loadCVar not found for dpm "
         "(cur="
      << dpm.getCurNodeID()
      << "). "
         "Returning condVar — points-to result may be unsound.\n";
  return dpm.getCondVar();
}

bool ContextDDA::isMustAlias(const CxtLocDPItem &loadDpm,
                             const CxtLocDPItem &storeDpm) const {
  (void)loadDpm;
  (void)storeDpm;
  return false;
}

bool ContextDDA::isCondCompatible(const ContextCond &cxt1,
                                  const ContextCond &cxt2,
                                  bool singleton) const {
  // SVF: context conditions of local/global vars are compatible; singleton =>
  // true.
  if (singleton)
    return true;
  int i = static_cast<int>(cxt1.cxtSize()) - 1;
  int j = static_cast<int>(cxt2.cxtSize()) - 1;
  const CallStrCxt &ctx1 = cxt1.getContexts();
  const CallStrCxt &ctx2 = cxt2.getContexts();
  for (; i >= 0 && j >= 0; --i, --j) {
    if (ctx1[static_cast<size_t>(i)] != ctx2[static_cast<size_t>(j)])
      return false;
  }
  return true;
}

bool ContextDDA::propagateViaObj(const CxtVar &storeObj,
                                 const CxtVar &loadObj) const {
  if (storeObj.get_id() != loadObj.get_id())
    return false;
  const uint32_t objId = storeObj.get_id();
  SVFG *svfg = getSVFG();
  // Conservative: if we lack object metadata, treat as singleton (compatible).
  bool singleton = true;
  if (svfg) {
    // SVF: distinguish context-sensitive heap allocations and stack objects in
    // recursive functions; otherwise treat as singleton objects.
    if (svfg->isHeapObject(objId) || svfg->isUnknownObject(objId)) {
      singleton = false;
    } else if (svfg->isStackObject(objId)) {
      const Value *v = svfg->getObjectValue(objId);
      if (const Instruction *inst = dyn_cast_or_null<Instruction>(v)) {
        const Function *f = inst->getFunction();
        if (flowDDA_ && flowDDA_->isRecursiveFunction(f))
          singleton = false;
      }
    }
  }
  return isCondCompatible(storeObj.get_cond(), loadObj.get_cond(), singleton);
}

void ContextDDA::initInsensitiveEdges() {
  insensitveEdges_.clear();
  SVFG *svfg = getSVFG();
  if (!svfg || (!insensitiveRecursion_ && !insensitiveCycle_))
    return;

  // Bug 6 fix: performSCCAnalysis takes the set of already-known insensitive
  // call/ret edges so it can exclude them from the SCC condensation (treating
  // them as cut edges). The old code passed an empty set, which caused the SCC
  // analysis to include recursion-marked edges in the condensation graph,
  // potentially producing an incorrect SCC decomposition that either missed
  // cycle edges or incorrectly merged SCCs.
  //
  // Correct approach (matching SVF ContextDDA::initInsensitiveEdges):
  //   1. First pass: collect all call/ret edges that are in recursion.
  //   2. Pass that set to performSCCAnalysis as the seed insensitive edges.
  //   3. Second pass: add any remaining call/ret edges that are in an SVFG SCC.
  CxtLocDPItem dummy(CxtVar(ContextCond(), 0), nullptr);

  SVFGStats::SVFGEdgeSet recursionInsensitive;
  if (insensitiveRecursion_) {
    for (const auto &pair : *svfg) {
      SVFGNode *node = pair.second;
      if (!node)
        continue;
      for (SVFGEdge *edge : node->getInEdges()) {
        if (!edge)
          continue;
        if (!isCallEdge(edge) && !isRetEdge(edge))
          continue;
        uint32_t csId = 0;
        if (isCallEdge(edge))
          csId = getCSIDAtCall(dummy, edge);
        else
          csId = getCSIDAtRet(dummy, edge);
        if (csId != 0 && isEdgeInRecursion(csId))
          recursionInsensitive.insert(edge);
      }
    }
    for (const SVFGEdge *e : recursionInsensitive)
      insensitveEdges_.insert(const_cast<SVFGEdge *>(e));
  }

  if (!insensitiveCycle_)
    return;

  // Step 2: run SCC analysis with the recursion edges excluded.
  SVFGStats stats(svfg);
  stats.performSCCAnalysis(recursionInsensitive);

  // Step 3: collect function pairs that participate in a value-flow cycle.
  //
  // Upstream SVF does not stop at "edge is in an SVFG SCC". It first records
  // all caller/callee function pairs that appear on such cycle edges, then
  // treats every call/ret edge between those function pairs as insensitive.
  //
  // This closure matters when a cycle only touches one inter edge of a given
  // caller/callee pair. Restricting insensitivity to the specific SCC edge can
  // still leave sibling call/ret edges context-sensitive, which diverges from
  // SVF and can reintroduce spurious context pruning on the same recursive
  // value-flow pair.
  struct FunctionPairHash {
    size_t operator()(
        const std::pair<const llvm::Function *, const llvm::Function *> &p)
        const noexcept {
      return std::hash<const llvm::Function *>()(p.first) ^
             (std::hash<const llvm::Function *>()(p.second) << 1);
    }
  };
  using FunctionPair = std::pair<const llvm::Function *, const llvm::Function *>;
  std::unordered_set<FunctionPair, FunctionPairHash> insensitiveFunPairs;

  for (const auto &pair : *svfg) {
    SVFGNode *node = pair.second;
    if (!node)
      continue;
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!edge)
        continue;
      if (!isCallEdge(edge) && !isRetEdge(edge))
        continue;
      if (insensitveEdges_.count(edge))
        continue; // already marked by recursion pass
      if (!stats.isEdgeInSVFGSCC(edge))
        continue;
      const llvm::Function *srcFun =
          edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
      const llvm::Function *dstFun =
          edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
      if (!srcFun || !dstFun)
        continue;
      insensitiveFunPairs.emplace(srcFun, dstFun);
      insensitiveFunPairs.emplace(dstFun, srcFun);
    }
  }

  // Step 4: mark every call/ret edge between the collected function pairs as
  // insensitive, matching SVF DDAPass::collectCxtInsenEdgeForVFCycle.
  for (const auto &pair : *svfg) {
    SVFGNode *node = pair.second;
    if (!node)
      continue;
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!edge)
        continue;
      if (!isCallEdge(edge) && !isRetEdge(edge))
        continue;
      if (insensitveEdges_.count(edge))
        continue;
      const llvm::Function *srcFun =
          edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
      const llvm::Function *dstFun =
          edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
      if (!srcFun || !dstFun)
        continue;
      if (insensitiveFunPairs.count(FunctionPair(srcFun, dstFun)) != 0)
        insensitveEdges_.insert(edge);
    }
  }
}

SVFGNode *ContextDDA::getDefNodeForValue(const llvm::Value *v) const {
  SVFG *svfg = getSVFG();
  if (!svfg || !v)
    return nullptr;
  if (SVFGNode *n = svfg->getValueNode(v))
    return n;
  if (const Instruction *inst = llvm::dyn_cast<Instruction>(v))
    return svfg->getDef(inst);
  return nullptr;
}

void ContextDDA::resolveFunPtr(const CxtLocDPItem &dpm) {
  SVFG *svfg = getSVFG();
  if (!svfg)
    return;
  const SVFGNode *node = dpm.getLoc();
  if (!node)
    return;

  // Match SVF DDAVFSolver::resolveFunPtr: trigger on any callsite-ret node
  // (ActualRet/ActualOut/InterPhi/MInterPhi) and any function-entry node
  // (FormalParm/FormalIn/InterPhi/MInterPhi), preserving the current context.
  if (const llvm::CallBase *cs = svfg->isCallSiteRetSVFGNode(node)) {
    if (!cs->getCalledFunction()) {
      const Value *calledOp = cs->getCalledOperand();
      if (calledOp && calledOp->getType()->isPointerTy()) {
        SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
        if (funPtrNode) {
          CxtVar funptrVar(dpm.getCondVar().get_cond(),
                           getTopLevelValueId(funPtrNode));
          CxtLocDPItem funPtrDpm(funptrVar, funPtrNode);
          findPT(funPtrDpm);
        }
      }
    }
  } else if (const llvm::Function *fun = svfg->isFunEntrySVFGNode(node)) {
    if (!fun->isDeclaration()) {
      const auto &indCS = svfg->getIndCallSitesInvokingCallee(fun);
      for (const llvm::CallBase *cs : indCS) {
        if (!cs || cs->getCalledFunction())
          continue;
        const Value *calledOp = cs->getCalledOperand();
        if (!calledOp || !calledOp->getType()->isPointerTy())
          continue;
        SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
        if (funPtrNode) {
          CxtVar funptrVar(dpm.getCondVar().get_cond(),
                           getTopLevelValueId(funPtrNode));
          CxtLocDPItem funPtrDpm(funptrVar, funPtrNode);
          findPT(funPtrDpm);
        }
      }
    }
  }
}

CxtLocDPItem ContextDDA::getDPImWithOldCond(const CxtLocDPItem &oldDpm,
                                            const CxtVar &var,
                                            const SVFGNode *loc) const {
  CxtLocDPItem dpm(oldDpm);
  dpm.setLocVar(loc, var.get_id());
  // Match SVF DDAVFSolver::getDPImWithOldCond: add load info for Store/Load
  // nodes. Bug 5 fix: guard with hasLoadDpm(oldDpm) before calling
  // getLoadDpm(oldDpm). The old code called getLoadDpm unconditionally for
  // Store nodes, triggering assert(false) + unsound fallback when oldDpm had no
  // associated load DPM (e.g. when the solver reaches a Store via a path from
  // EntryChi/FormalIn).
  ContextDDA *nonConstThis = const_cast<ContextDDA *>(this);
  if (llvm::isa<StoreSVFGNode>(loc) || llvm::isa<StoreChiSVFGNode>(loc)) {
    if (hasLoadDpm(oldDpm))
      nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), var);
  }
  if (llvm::isa<LoadSVFGNode>(loc) || llvm::isa<LoadMuSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, oldDpm, var);
  return dpm;
}

void ContextDDA::handleAddr(CxtPtSet &pts, const CxtLocDPItem &dpm,
                            const AddrSVFGNode *addr) {
  ContextCond cond = dpm.getCond();
  if (!addr || !flowDDA_)
    return;
  SVFGBuilder *builder = flowDDA_->getSVFGBuilder();
  SVFG *svfg = getSVFG();
  SVFGNodeBS objIds;
  if (const uint32_t canonicalObjId = addr->getObjectId())
    objIds.insert(canonicalObjId);
  else if (const Value *v = addr->getValue())
    objIds = flowDDA_->getObjectIdsForValue(v);
  for (uint32_t id : objIds) {
    // SVF field-insensitivity check
    if (svfg && svfg->isFieldInsensitiveObject(id) && builder) {
      uint32_t fiObj = builder->getOrCreateFIObjId(id);
      if (fiObj != 0) {
        pts.insert(CxtVar(cond, fiObj));
        continue;
      }
    }
    pts.insert(CxtVar(cond, id));
  }
}

CxtPtSet ContextDDA::processGepPts(const GepSVFGNode *gep,
                                   const CxtPtSet &srcPts) {
  if (!gep || !gep->getValue() || !isa<GetElementPtrInst>(gep->getValue()))
    return srcPts;

  const auto *gi = cast<GetElementPtrInst>(gep->getValue());
  CxtPtSet tmpDstPts;

  // Match SVF ContextDDA::processGepPts logic
  const bool isVariantFieldGep = !gi->hasAllConstantIndices();
  SVFGBuilder *builder = flowDDA_ ? flowDDA_->getSVFGBuilder() : nullptr;
  for (const CxtVar &ptd : srcPts) {
    if (ptd.get_id() == 0) {
      tmpDstPts.insert(ptd);
      continue;
    }
    uint32_t newObjId = 0;
    if (builder) {
      if (isVariantFieldGep)
        newObjId = builder->getOrCreateFIObjId(ptd.get_id());
      else
        newObjId = builder->getGepObjectId(ptd.get_id(), gi);
    }
    if (newObjId == 0)
      newObjId = ptd.get_id();
    tmpDstPts.insert(CxtVar(ptd.get_cond(), newObjId));
  }

  return tmpDstPts;
}

bool ContextDDA::isStrongUpdate(const CxtPtSet &dstPts,
                                const StoreSVFGNode *store) {
  if (dstPts.size() != 1)
    return false;
  (void)store;

  const CxtVar &var = *dstPts.begin();
  uint32_t objId = var.get_id();
  SVFG *svfg = getSVFG();
  if (!svfg)
    return false;
  // Match SVF ContextDDA::isStrongUpdate logic
  if (svfg->isUnknownObject(objId))
    return false;
  if (svfg->isHeapObject(objId)) {
    // Lotus's SVFG object model does not currently distinguish SVF's
    // DummyObjVar-style concrete heap cells from ordinary heap allocations.
    // Upstream only allows the former to strong-update under additional
    // context/loop checks; treating all heap objects as eligible would be
    // unsound. Stay conservative until the SVFG carries that distinction.
    return false;
  }
  if (svfg->isArrayObject(objId))
    return false;
  if (svfg->isFieldInsensitiveObject(objId))
    return false;
  // Local variables in recursion
  if (svfg->isStackObject(objId)) {
    const Value *v = svfg->getObjectValue(objId);
    if (const Instruction *inst = dyn_cast_or_null<Instruction>(v)) {
      const Function *f = inst->getFunction();
      if (flowDDA_ && flowDDA_->isRecursiveFunction(f))
        return false;
    }
  }
  return true;
}

const CxtPtSet &ContextDDA::computeDDAPts(const CxtVar &cxtVar) {
  if (getDDAStat())
    getDDAStat()->numQueries++;
  resetQuery();
  DPItem::setMaxBudget(FlowDDA::getDefaultMaxBudget());
  SVFG *svfg = getSVFG();
  if (!svfg) {
    static const CxtPtSet empty;
    return empty;
  }
  SVFGNode *defNode = svfg->getCanonicalDefNodeForDDAId(cxtVar.get_id());
  if (!defNode) {
    static const CxtPtSet empty;
    return empty;
  }
  CxtLocDPItem dpm(cxtVar, defNode);
  (void)findPT(dpm);
  if (isOutOfBudget()) {
    if (getDDAStat())
      getDDAStat()->numOutOfBudgetQueries++;
    insertOutOfBudgetDpm(dpm);
    handleOutOfBudgetDpm(dpm);
  }
  return getCachedPointsTo(dpm);
}

CxtPtSet ContextDDA::computeDDAPts(const llvm::Value *ptr) {
  SVFG *svfg = getSVFG();
  if (!svfg || !ptr || !ptr->getType()->isPointerTy())
    return CxtPtSet{};
  const Value *v = ptr->stripPointerCasts();
  SVFGNode *defNode = svfg->getValueNode(v);
  if (!defNode) {
    if (const Instruction *inst = llvm::dyn_cast<Instruction>(v))
      defNode = svfg->getDef(inst);
    if (!defNode)
      return CxtPtSet{};
  }
  ContextCond cxt;
  CxtVar cxtVar(cxt, getTopLevelValueId(defNode));
  return computeDDAPts(cxtVar);
}

void ContextDDA::answerQueries() {
  if (!client_ || !flowDDA_ || !getSVFG())
    return;
  client_->answerQueries(this);
}

bool ContextDDA::mayAlias(const llvm::Value *v1, const llvm::Value *v2) {
  if (!getSVFG() || !v1 || !v2)
    return true;
  const llvm::Value *p1 = v1->stripPointerCasts();
  const llvm::Value *p2 = v2->stripPointerCasts();
  if (!p1->getType()->isPointerTy() || !p2->getType()->isPointerTy())
    return false;
  if (p1 == p2)
    return true;

  const CxtPtSet pts1 = computeDDAPts(p1);
  const CxtPtSet pts2 = computeDDAPts(p2);
  for (const CxtVar &lhs : pts1) {
    for (const CxtVar &rhs : pts2) {
      if (propagateViaObj(lhs, rhs))
        return true;
    }
  }
  return false;
}
