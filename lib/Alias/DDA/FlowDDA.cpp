//===- FlowDDA.cpp -- Flow-sensitive demand-driven analysis ---------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// Implements value-flow-based demand-driven pointer analysis following
// SVF's FlowDDA / DDAVFSolver (FSE'16, TSE'18).
//
//===----------------------------------------------------------------------===//

#include "Alias/DDA/FlowDDA.h"

#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/DDAStat.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <stack>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Casting.h>

using namespace llvm;
using namespace lotus::analysis;

namespace {

// Conservative fallback prefers pointer-typed SSA values associated with the
// query state. We consider both the current location's value and the object-id
// mapped value, then union AserPTA-backed object IDs for all candidates.
static std::vector<const Value *>
collectFallbackPointerValues(const SVFG *svfg, const LocDPItem &dpm) {
  std::vector<const Value *> values;
  if (const SVFGNode *loc = dpm.getLoc()) {
    if (const Value *locVal = loc->getValue()) {
      if (locVal->getType()->isPointerTy())
        values.push_back(locVal);
    }
  }
  if (svfg) {
    if (const Value *objVal = svfg->getObjectValue(dpm.getCurNodeID())) {
      if (objVal->getType()->isPointerTy() &&
          std::find(values.begin(), values.end(), objVal) == values.end()) {
        values.push_back(objVal);
      }
    }
  }
  return values;
}

} // namespace

uint32_t DPItem::maximumBudget = 100000u;
uint32_t FlowDDA::defaultMaxBudget_ = 100000u;

FlowDDA::FlowDDA() {
  ddaStat_ = std::make_unique<DDAStat>(this);
  setDDAStat(ddaStat_.get());
}

FlowDDA::~FlowDDA() = default;

void FlowDDA::answerQueries() {
  if (client_) {
    client_->setSVFG(svfg_.get());
    if (module_)
      client_->setModule(module_);
    client_->answerQueries(this);
  }
}

bool FlowDDA::handleBKCondition(LocDPItem &dpm, SVFGEdge *edge) {
  if (client_)
    client_->handleStatement(edge->getSrcNode(), dpm.getCurNodeID());
  return true;
}

void FlowDDA::handleOutOfBudgetDpm(const LocDPItem &dpm) {
  // SVF-style conservative downgrade: seed cache with fallback points-to and
  // remember this DPM as out-of-budget so later traversals do not recurse.
  const PtsSet conservativePts = getConservativeCPts(dpm);
  if (!conservativePts.empty())
    DDAVFSolver<uint32_t, std::unordered_set<uint32_t>, LocDPItem,
                FlowDDA>::updateCachedPointsTo(dpm, conservativePts);
  DDAVFSolver<uint32_t, std::unordered_set<uint32_t>, LocDPItem,
              FlowDDA>::addOutOfBudgetDpm(dpm);
}

bool FlowDDA::run(Module &M) {
  if (initialized_)
    return true;
  try {
    icfg_ = std::make_unique<::ICFG>();
    icfgBuilder_ = std::make_unique<::ICFGBuilder>(icfg_.get());
    icfgBuilder_->build(&M);
    SVFGBuilderConfig cfg;
    // Match SVF FlowDDA/ContextDDA: indirect-call edges are inserted on-the-fly
    // when function-pointer points-to is discovered.
    cfg.resolveIndirectCalls = false;
    svfgBuilder_ = std::make_unique<SVFGBuilder>(cfg);
    SVFG *built = svfgBuilder_->build(icfg_.get());
    if (!built) {
      icfg_.reset();
      icfgBuilder_.reset();
      svfgBuilder_.reset();
      return false;
    }
    svfg_.reset(built);
    module_ = &M;
    buildRecursionInfo();
    buildLoopInfo();
  } catch (const std::exception &) {
    icfg_.reset();
    icfgBuilder_.reset();
    svfgBuilder_.reset();
    svfg_.reset();
    return false;
  }
  initialized_ = true;
  return true;
}

uint32_t FlowDDA::getTopLevelValueId(const SVFGNode *node) const {
  if (!node)
    return 0;
  if (node->hasValueId())
    return node->getValueId();
  return node->getId();
}

void FlowDDA::unionDDAPts(PtsSet &target, const PtsSet &source) {
  for (uint32_t id : source)
    target.insert(id);
}

bool FlowDDA::unionDDAPts(const LocDPItem &dpm, const PtsSet &pts) {
  auto &cache =
      isTopLevelPtrStmt(dpm.getLoc()) ? dpmToTLPtsMap_ : dpmToADPtsMap_;
  auto it = cache.find(dpm);
  if (it == cache.end()) {
    cache[dpm] = pts;
    return !pts.empty();
  }
  size_t oldSize = it->second.size();
  for (uint32_t id : pts)
    it->second.insert(id);
  return it->second.size() != oldSize;
}

void FlowDDA::resetQueryLoadMaps() {
  dpmToLoadDpmMap_.clear();
  dpmToLoadCVarMap_.clear();
}

void FlowDDA::insertOutOfBudgetDpm(const LocDPItem &dpm) {
  outOfBudgetDpms_.insert(dpm);
}

bool FlowDDA::isOutOfBudgetDpm(const LocDPItem &dpm) const {
  return outOfBudgetDpms_.count(dpm) != 0;
}

const FlowDDA::PtsSet &FlowDDA::getEmptyCPtSetRef() const {
  static const PtsSet empty;
  return empty;
}

void FlowDDA::setDpmLocVar(LocDPItem &dpm, SVFGNode *src, uint32_t ptrNodeId) {
  dpm.setLocVar(src, ptrNodeId);
}

void FlowDDA::connectIndirectCallees(const LocDPItem &dpm, const PtsSet &funPts,
                                     std::vector<SVFGEdge *> &newEdges) {
  const auto &indCallSites = svfg_->getIndCallSites(dpm.getCurNodeID());
  for (const CallBase *cs : indCallSites) {
    if (!cs)
      continue;
    for (uint32_t objId : funPts) {
      if (objId == 0)
        continue;
      const Value *v = svfg_->getObjectValue(objId);
      const Function *callee = dyn_cast_or_null<Function>(v);
      if (!callee || callee->isDeclaration())
        continue;
      (void)svfgBuilder_->connectCallSiteToCalleeOnTheFly(svfg_.get(), cs,
                                                          callee, newEdges);
    }
  }
}

void FlowDDA::forEachObjId(const PtsSet &pts,
                           std::function<void(uint32_t)> callback) const {
  for (uint32_t id : pts)
    callback(id);
}

void FlowDDA::forEachElementInCPtSet(
    const PtsSet &pts, std::function<void(uint32_t, uint32_t)> callback) const {
  for (uint32_t id : pts)
    callback(id, id);
}

bool FlowDDA::propagateViaObj(uint32_t storeObj, uint32_t loadObj) const {
  return storeObj == loadObj;
}

void FlowDDA::resolveFunPtr(const LocDPItem &dpm) {
  const SVFGNode *node = dpm.getLoc();
  if (!node || !svfg_)
    return;
  // Match SVF DDAVFSolver::resolveFunPtr: trigger on any callsite-ret node
  // (ActualRet/ActualOut/InterPhi/MInterPhi) and any function-entry node
  // (FormalParm/FormalIn/InterPhi/MInterPhi), not just ActualRet/FormalParm.
  if (const llvm::CallBase *cs = svfg_->isCallSiteRetSVFGNode(node)) {
    if (!cs->getCalledFunction()) {
      const Value *calledOp = cs->getCalledOperand();
      if (calledOp && calledOp->getType()->isPointerTy()) {
        SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
        if (funPtrNode) {
          LocDPItem funPtrDpm(getTopLevelValueId(funPtrNode), funPtrNode);
          findPT(funPtrDpm);
        }
      }
    }
  } else if (const llvm::Function *fun = svfg_->isFunEntrySVFGNode(node)) {
    if (!fun->isDeclaration()) {
      const auto &indCS = svfg_->getIndCallSitesInvokingCallee(fun);
      for (const llvm::CallBase *cs : indCS) {
        if (!cs || cs->getCalledFunction())
          continue;
        const Value *calledOp = cs->getCalledOperand();
        if (!calledOp || !calledOp->getType()->isPointerTy())
          continue;
        SVFGNode *funPtrNode = getDefNodeForValue(calledOp);
        if (funPtrNode) {
          LocDPItem funPtrDpm(getTopLevelValueId(funPtrNode), funPtrNode);
          findPT(funPtrDpm);
        }
      }
    }
  }
}

void FlowDDA::addLoadDpmAndCVar(const LocDPItem &dpm, const LocDPItem &loadDpm,
                                uint32_t loadCVarObjId) {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    it->second = loadDpm;
  else
    dpmToLoadDpmMap_.emplace(dpm, loadDpm);
  dpmToLoadCVarMap_[dpm] = loadCVarObjId;
}

bool FlowDDA::hasLoadDpm(const LocDPItem &dpm) const {
  return dpmToLoadDpmMap_.find(dpm) != dpmToLoadDpmMap_.end();
}

LocDPItem FlowDDA::getLoadDpm(const LocDPItem &dpm) const {
  auto it = dpmToLoadDpmMap_.find(dpm);
  if (it != dpmToLoadDpmMap_.end())
    return it->second;
  // Callers must guard with hasLoadDpm() before calling this. Reaching here
  // means the load/store DPM linkage was never established, which indicates a
  // logic error in getDPImWithOldCond or the Store handler. Assert in debug
  // builds; in release, return the dpm itself so the solver can continue
  // (conservative: treats the store as a self-alias, which is sound but
  // imprecise) and emit a diagnostic so the bug is visible.
  assert(false && "FlowDDA::getLoadDpm: loadDpm not found; "
                  "caller should have checked hasLoadDpm() first");
  llvm::errs() << "[DDA bug] FlowDDA::getLoadDpm: loadDpm not found for dpm "
                  "(cur="
               << dpm.getCurNodeID()
               << "). "
                  "Returning self — points-to result may be unsound.\n";
  return dpm;
}

uint32_t FlowDDA::getLoadCVar(const LocDPItem &dpm) const {
  auto it = dpmToLoadCVarMap_.find(dpm);
  if (it != dpmToLoadCVarMap_.end())
    return it->second;
  // Same contract as getLoadDpm: callers must check hasLoadDpm() first.
  assert(false && "FlowDDA::getLoadCVar: loadCVar not found; "
                  "caller should have checked hasLoadDpm() first");
  llvm::errs() << "[DDA bug] FlowDDA::getLoadCVar: loadCVar not found for dpm "
                  "(cur="
               << dpm.getCurNodeID()
               << "). "
                  "Returning curNodeID — points-to result may be unsound.\n";
  return dpm.getCurNodeID();
}

bool FlowDDA::isMustAlias(const LocDPItem &loadDpm,
                          const LocDPItem &storeDpm) const {
  (void)loadDpm;
  (void)storeDpm;
  // Match upstream SVF DDAVFSolver default: FlowDDA does not implement
  // must-alias.
  return false;
}

bool FlowDDA::isHeapCondMemObj(uint32_t objId,
                               const StoreSVFGNode *store) const {
  (void)store;
  if (objId == 0 || !svfg_)
    return false;
  // Match upstream SVF FlowDDA: exclude heap/dummy objects from strong update.
  if (svfg_->isUnknownObject(objId))
    return true;
  return svfg_->isHeapObject(objId);
}

bool FlowDDA::isLocalCVarInRecursion(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  const Value *v = svfg_->getObjectValue(objId);
  if (!v)
    return false;
  const llvm::Instruction *inst = dyn_cast<llvm::Instruction>(v);
  if (!inst)
    return false;
  const llvm::Function *f = inst->getFunction();
  return f && recursiveFunctions_.count(f) != 0;
}

bool FlowDDA::isArrayCondMemObj(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  return svfg_->isArrayObject(objId);
}

bool FlowDDA::isFieldInsenCondMemObj(uint32_t objId) const {
  if (!svfg_ || objId == 0)
    return false;
  return svfg_->isFieldInsensitiveObject(objId);
}

SVFGNode *FlowDDA::getDefNodeForValue(const Value *v) const {
  if (!svfg_ || !v)
    return nullptr;
  if (SVFGNode *n = svfg_->getValueNode(v))
    return n;
  if (const Instruction *inst = dyn_cast<Instruction>(v))
    return svfg_->getDef(inst);
  return nullptr;
}

LocDPItem FlowDDA::getDPImWithOldCond(const LocDPItem &oldDpm, uint32_t objId,
                                      const SVFGNode *loc) const {
  LocDPItem dpm(oldDpm);
  dpm.setLocVar(loc, objId);
  // Match SVF DDAVFSolver::getDPImWithOldCond: add load info for Store/Load
  // nodes. Bug 5 fix (FlowDDA side): guard with hasLoadDpm(oldDpm) before
  // calling getLoadDpm(oldDpm) for Store nodes. The old code called getLoadDpm
  // unconditionally, triggering assert(false) + unsound fallback when oldDpm
  // had no associated load DPM (e.g. path from EntryChi/FormalIn to a Store).
  FlowDDA *nonConstThis = const_cast<FlowDDA *>(this);
  if (isa<StoreSVFGNode>(loc) || isa<StoreChiSVFGNode>(loc)) {
    if (hasLoadDpm(oldDpm))
      nonConstThis->addLoadDpmAndCVar(dpm, getLoadDpm(oldDpm), objId);
  }
  if (isa<LoadSVFGNode>(loc) || isa<LoadMuSVFGNode>(loc))
    nonConstThis->addLoadDpmAndCVar(dpm, oldDpm, objId);
  return dpm;
}

bool FlowDDA::isDirectEdge(SVFGEdge *e) {
  if (!e)
    return false;
  if (isIndirectEdge(e))
    return false;
  const SVFGEdgeK k = e->getEdgeKind();
  if (isDirectVFGEdge(k))
    return true;
  // Lotus-specific direct-flow edges not present in SVF's minimal edge lattice.
  return k == SVFGEdgeK::ParamCall || k == SVFGEdgeK::ParamRet ||
         k == SVFGEdgeK::IntraCmp || k == SVFGEdgeK::IntraBranch;
}

bool FlowDDA::isIndirectEdge(SVFGEdge *e) {
  if (!e)
    return false;
  const SVFGEdgeK k = e->getEdgeKind();
  if (isIndirectVFGEdge(k)) {
    return true;
  }
  // Memory SSA builder uses IntraPhi/IntraCopy between memory nodes. Treat
  // those as indirect to match SVF's IndirectSVFGEdge semantics.
  if (k == SVFGEdgeK::IntraPhi || k == SVFGEdgeK::IntraCopy ||
      k == SVFGEdgeK::IntraDirect) {
    const SVFGNode *src = e->getSrcNode();
    const SVFGNode *dst = e->getDstNode();
    return (src && src->isMemNode()) || (dst && dst->isMemNode());
  }
  return false;
}

void FlowDDA::handleAddr(PtsSet &pts, const LocDPItem &,
                         const AddrSVFGNode *addr) {
  if (!addr)
    return;
  SVFGNodeBS objIds;
  if (const uint32_t canonicalObjId = addr->getObjectId())
    objIds.insert(canonicalObjId);
  else if (const Value *v = addr->getValue())
    objIds = getObjectIdsForValue(v);
  for (uint32_t id : objIds) {
    // SVF field-insensitivity check: if isFieldInsensitive(srcID) srcID =
    // getFIObjVar(srcID)
    if (svfg_ && svfg_->isFieldInsensitiveObject(id) && svfgBuilder_) {
      uint32_t fiObj = svfgBuilder_->getOrCreateFIObjId(id);
      if (fiObj != 0) {
        pts.insert(fiObj);
        continue;
      }
    }
    pts.insert(id);
  }
}

FlowDDA::PtsSet FlowDDA::processGepPts(const GepSVFGNode *gep,
                                       const PtsSet &srcPts) {
  if (!gep || !gep->getValue() || !isa<GetElementPtrInst>(gep->getValue()))
    return srcPts;

  const auto *gi = cast<GetElementPtrInst>(gep->getValue());
  PtsSet tmpDstPts;

  // Match SVF FlowDDA::processGepPts logic
  const bool isVariantFieldGep = !gi->hasAllConstantIndices();
  for (uint32_t objId : srcPts) {
    if (objId == 0) {
      tmpDstPts.insert(objId);
      continue;
    }
    uint32_t gepObjId = 0;
    if (svfgBuilder_) {
      if (isVariantFieldGep)
        gepObjId = svfgBuilder_->getOrCreateFIObjId(objId);
      else
        gepObjId = svfgBuilder_->getGepObjectId(objId, gi);
    }
    if (gepObjId == 0)
      gepObjId = objId;
    tmpDstPts.insert(gepObjId);
  }

  return tmpDstPts;
}

bool FlowDDA::isStrongUpdate(const PtsSet &dstPts, const StoreSVFGNode *store) {
  if (dstPts.size() != 1)
    return false;
  const uint32_t objId = *dstPts.begin();
  // Match SVF DDAVFSolver::isStrongUpdate: exclude heap, array,
  // field-insensitive, recursion
  if (isHeapCondMemObj(objId, store))
    return false;
  if (isArrayCondMemObj(objId))
    return false;
  if (isFieldInsenCondMemObj(objId))
    return false;
  if (isLocalCVarInRecursion(objId))
    return false;
  return true;
}

FlowDDA::PtsSet FlowDDA::getPointsTo(const Value *ptr) {
  PtsSet result;
  if (ddaStat_)
    ddaStat_->numQueries++;
  if (!initialized_ || !svfg_ || !ptr || !ptr->getType()->isPointerTy())
    return result;

  const Value *v = ptr->stripPointerCasts();
  auto cacheIt = ptsCache_.find(v);
  if (cacheIt != ptsCache_.end())
    return cacheIt->second;

  SVFGNode *defNode = svfg_->getValueNode(v);
  if (!defNode) {
    if (const Instruction *inst = dyn_cast<Instruction>(v))
      defNode = svfg_->getDef(inst);
    if (!defNode)
      return result;
  }

  // Keep cross-query memoization in sync with SVF's design: the DPM caches are
  // preserved across queries, so a Value* query can also be memoized until the
  // SVFG itself changes. SVFG mutations still invalidate ptsCache_ via
  // onIndirectEdgesAdded().
  resetQuery();
  LocDPItem::setMaxBudget(defaultMaxBudget_);
  LocDPItem dpm(getTopLevelValueId(defNode), defNode);
  (void)findPT(dpm);
  if (isOutOfBudget()) {
    if (ddaStat_)
      ddaStat_->numOutOfBudgetQueries++;
    handleOutOfBudgetDpm(dpm);
  }
  result = getCachedPointsTo(dpm);
  ptsCache_[v] = result;
  return result;
}

FlowDDA::PtsSet FlowDDA::getPointsToCached(const Value *ptr) {
  auto it = ptsCache_.find(ptr);
  if (it != ptsCache_.end())
    return it->second;
  PtsSet result = getPointsTo(ptr);
  ptsCache_[ptr] = result;
  return result;
}

bool FlowDDA::getPointsToSet(const Value *ptr,
                             std::vector<const Value *> &out) {
  out.clear();
  PtsSet pts = getPointsTo(ptr);
  if (!svfg_)
    return !pts.empty();
  for (uint32_t objId : pts) {
    if (const Value *v = svfg_->getObjectValue(objId))
      out.push_back(v);
  }
  return !out.empty();
}

void FlowDDA::buildRecursionInfo() {
  recursiveFunctions_.clear();
  if (!module_)
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
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const llvm::CallBase *cb = dyn_cast<llvm::CallBase>(&I);
        if (!cb)
          continue;
        std::vector<const llvm::Function *> callees;
        if (const llvm::Function *direct = cb->getCalledFunction()) {
          if (!direct->isDeclaration())
            callees.push_back(direct);
        } else if (svfg_) {
          const auto &connected = svfg_->getConnectedCallees(cb);
          for (const llvm::Function *callee : connected)
            callees.push_back(callee);
          if (svfgBuilder_)
            appendUniqueCallees(callees, svfgBuilder_->getIndirectCallTargets(cb));
        } else if (svfgBuilder_) {
          callees = svfgBuilder_->getIndirectCallTargets(cb);
        }
        for (const llvm::Function *callee : callees) {
          if (!callee || callee->isDeclaration())
            continue;
          callGraph[&F].push_back(callee);
        }
      }
  }
  // Bug 8 fix: the old implementation used a recursive std::function for
  // Tarjan's SCC algorithm, which overflows the system stack on programs with
  // deep call chains (e.g. SPEC2006 benchmarks). Replace with an explicit
  // worklist-based iterative Tarjan's algorithm.
  std::unordered_map<const llvm::Function *, uint32_t> index, lowlink;
  std::stack<const llvm::Function *> stk;
  std::unordered_set<const llvm::Function *> onStack;
  uint32_t nextIndex = 0;
  std::vector<std::set<const llvm::Function *>> sccs;
  std::unordered_map<const llvm::Function *, size_t> funcToScc;

  // Iterative Tarjan: each worklist entry is (function, iterator into its
  // callee list). When the iterator is at begin() the node is being visited
  // for the first time; when it is at end() we are returning from all callees.
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
      // First visit: assign index/lowlink and push onto SCC stack.
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
      // Process next unvisited callee.
      bool pushed = false;
      while (frame.it != frame.end) {
        const llvm::Function *callee = *frame.it;
        ++frame.it;
        if (!index.count(callee)) {
          // Push callee frame; will update lowlink[f] on return.
          worklist.push({callee, {}, {}});
          pushed = true;
          break;
        } else if (onStack.count(callee)) {
          lowlink[f] = std::min(lowlink[f], index[callee]);
        }
      }
      if (pushed)
        continue;
      // All callees processed: update parent's lowlink and check SCC root.
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
        size_t id = sccs.size();
        sccs.push_back(std::move(scc));
        for (const llvm::Function *g : sccs[id])
          funcToScc[g] = id;
      }
    }
  };

  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    visit(&F);
  }

  for (const auto &scc : sccs) {
    if (scc.size() > 1) {
      // Multi-function SCC: all members are recursive.
      for (const llvm::Function *f : scc)
        recursiveFunctions_.insert(f);
    } else if (scc.size() == 1) {
      // Single-function SCC: check for self-recursion (self-edge in call
      // graph).
      const llvm::Function *f = *scc.begin();
      auto it = callGraph.find(f);
      if (it != callGraph.end()) {
        for (const llvm::Function *callee : it->second) {
          if (callee == f) {
            recursiveFunctions_.insert(f);
            break;
          }
        }
      }
    }
  }
}

void FlowDDA::buildLoopInfo() {
  loopInfoMap_.clear();
  if (!module_)
    return;
  for (const llvm::Function &F : *module_) {
    if (F.isDeclaration())
      continue;
    llvm::DominatorTree DT(const_cast<llvm::Function &>(F));
    auto LI = std::make_unique<llvm::LoopInfo>();
    LI->analyze(DT);
    loopInfoMap_[&F] = std::move(LI);
  }
}

bool FlowDDA::mayAlias(const Value *v1, const Value *v2) {
  if (!initialized_ || !v1 || !v2)
    return true;
  const Value *p1 = v1->stripPointerCasts();
  const Value *p2 = v2->stripPointerCasts();
  if (!p1->getType()->isPointerTy() || !p2->getType()->isPointerTy())
    return false;
  if (p1 == p2)
    return true;
  PtsSet pts1 = getPointsToCached(p1);
  PtsSet pts2 = getPointsToCached(p2);
  for (uint32_t id : pts1) {
    if (pts2.count(id))
      return true;
  }
  return false;
}

bool FlowDDA::mayNull(const Value *ptr) {
  if (!ptr || !ptr->getType()->isPointerTy())
    return false;
  PtsSet pts = getPointsToCached(ptr->stripPointerCasts());
  if (pts.count(0) != 0)
    return true;
  if (svfg_) {
    for (uint32_t id : pts) {
      if (svfg_->isUnknownObject(id))
        return true;
    }
  }
  return false;
}

FlowDDA::PtsSet FlowDDA::getConservativeCPts(const LocDPItem &dpm) const {
  if (!svfg_) {
    if (svfgBuilder_) {
      auto *builder = const_cast<SVFGBuilder *>(svfgBuilder_.get());
      const uint32_t unknown = builder->getUnknownObjId();
      if (unknown != 0)
        return PtsSet{unknown};
    }
    return PtsSet{};
  }
  const std::vector<const Value *> values =
      collectFallbackPointerValues(svfg_.get(), dpm);
  if (values.empty()) {
    if (svfgBuilder_) {
      auto *builder = const_cast<SVFGBuilder *>(svfgBuilder_.get());
      const uint32_t unknown = builder->getUnknownObjId();
      if (unknown != 0)
        return PtsSet{unknown};
    }
    return PtsSet{};
  }
  PtsSet out;
  for (const Value *v : values) {
    SVFGNodeBS ids = getObjectIdsForValue(v);
    for (uint32_t id : ids)
      out.insert(id);
  }
  if (out.empty() && svfgBuilder_) {
    auto *builder = const_cast<SVFGBuilder *>(svfgBuilder_.get());
    const uint32_t unknown = builder->getUnknownObjId();
    if (unknown != 0)
      out.insert(unknown);
  }
  return out;
}

SVFGNodeBS FlowDDA::getObjectIdsForValue(const Value *v) const {
  SVFGNodeBS ids;
  if (!v || !v->getType()->isPointerTy())
    return ids;
  if (svfgBuilder_) {
    ids = svfgBuilder_->getObjectIdsForValue(v);
    if (!ids.empty()) {
      return ids;
    }
  }
  if (svfg_) {
    const uint32_t id = svfg_->getObjectId(v);
    if (id != 0)
      ids.insert(id);
  }
  return ids;
}

bool FlowDDA::isRecursiveFunction(const Function *f) const {
  if (!f)
    return false;
  return recursiveFunctions_.count(f) != 0;
}

bool FlowDDA::isInLoop(const llvm::Instruction *inst) const {
  if (!inst)
    return false;
  const llvm::Function *f = inst->getFunction();
  if (!f)
    return false;
  auto it = loopInfoMap_.find(f);
  if (it == loopInfoMap_.end() || !it->second)
    return false;
  const llvm::BasicBlock *bb = inst->getParent();
  return bb && it->second->getLoopFor(bb) != nullptr;
}

bool FlowDDA::isTopLevelPtrStmt(const SVFGNode *stmt) const {
  // Match SVF DDAVFSolver::isTopLevelPtrStmt: Store and MRSVFG are not
  // top-level
  if (!stmt)
    return false;
  return stmt->getNodeKind() != SVFGK::Store && !stmt->isMemNode();
}
