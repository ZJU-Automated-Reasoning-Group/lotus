//===- DDAVFSolver.h -- Value-flow demand-driven solver (SVF-style) -------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// DDAVFSolver<Derived>: Shared value-flow backward solver for demand-driven
// pointer analysis. FlowDDA and ContextDDA instantiate this template with
// their CVar, CPtSet, and DPIm types (flow-sensitive only vs
// context-sensitive). Matches SVF's DDAVFSolver design (FSE'16, TSE'18).
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DDAStat.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <functional>
#include <map>
#include <set>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>

namespace lotus {
namespace analysis {

/// Value-flow backward solver template. Derived must define:
/// - CVar, CPtSet, DPIm (types)
/// - getSVFG(), getSVFGBuilder(), getDefNodeForValue(), getTopLevelValueId(),
///   getObjectIdsForValue()
/// - isDirectEdge(), isIndirectEdge(), handleBKCondition(),
/// getConservativeCPts()
/// - handleAddr(), processGepPts(), isStrongUpdate(), getPtrNodeID(),
/// addDDAPts()
/// - unionDDAPts(target, source), unionDDAPts(dpm, pts), getDPImWithOldCond()
/// - resolveFunPtr(), isTopLevelPtrStmt()
/// - hasLoadDpm(), getLoadDpm(), getLoadCVar(), isMustAlias(),
/// propagateViaObj()
/// - forEachElementInCPtSet(), getEmptyCPtSetRef()
/// - setDpmLocVar(), addLoadDpmAndCVar(), connectIndirectCallees(),
/// onIndirectEdgesAdded()
/// - insertOutOfBudgetDpm(), isOutOfBudgetDpm()
template <typename CVar, typename CPtSet, typename DPIm, typename Derived>
class DDAVFSolver {
public:
  DDAVFSolver() : ddaStat_(nullptr), numSteps_(0), outOfBudget_(false) {}
  virtual ~DDAVFSolver() = default;

  Derived &derived() { return *static_cast<Derived *>(this); }
  const Derived &derived() const { return *static_cast<const Derived *>(this); }

  void setDDAStat(DDAStat *s) { ddaStat_ = s; }
  DDAStat *getDDAStat() const { return ddaStat_; }

  /// Core query primitive (memoized DFS on SVFG backward edges).
  ///
  /// - Reuses previously-computed points-to when `dpm` is already visited.
  /// - Otherwise evaluates the current SVFG statement, updates the cache, and
  ///   triggers forward re-computation for dependents when new facts appear.
  /// - The concrete statement semantics are implemented in
  ///   `handleSingleStatement` below.
  const CPtSet &findPT(const DPIm &dpm) {
    if (isbkVisited(dpm))
      return getCachedPointsTo(dpm);
    markbkVisited(dpm);
    addDpmToLoc(dpm);
    if (!testOutOfBudget(dpm)) {
      if (ddaStat_)
        ddaStat_->numOfDPM++;
      CPtSet pts;
      handleSingleStatement(dpm, pts);
      updateCachedPointsTo(dpm, pts);
    }
    return getCachedPointsTo(dpm);
  }

  /// Reset per-query working state.
  ///
  /// Cache maps (`dpmToTLPtsMap_` / `dpmToADPtsMap_`) are intentionally kept
  /// across queries to mimic SVF's reuse strategy; only traversal-local state
  /// and step budget counters are cleared.
  void resetQuery() {
    if (outOfBudget_)
      OOBResetVisited();
    locToDpmSetMap_.clear();
    derived().resetQueryLoadMaps();
    numSteps_ = 0;
    outOfBudget_ = false;
    if (ddaStat_)
      ddaStat_->numOfStep = 0;
  }

  bool isOutOfBudget() const { return outOfBudget_; }
  void setOutOfBudget(bool b) { outOfBudget_ = b; }

protected:
  /// Evaluate one SVFG statement under demand item `dpm`.
  ///
  /// This implements the same high-level transfer decomposition as SVF:
  /// - Address/copy-like statements: direct backtrace
  /// - GEP: backtrace then field/offset filtering
  /// - Load/store: split into nested sub-queries on memory objects
  /// - Memory nodes: indirect backtrace
  void handleSingleStatement(const DPIm &dpm, CPtSet &pts) {
    const SVFGNode *node = dpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    derived().resolveFunPtr(dpm);

    switch (node->getNodeKind()) {
    case SVFGK::Addr:
      derived().handleAddr(pts, dpm, llvm::cast<AddrSVFGNode>(node));
      break;
    case SVFGK::Copy:
    case SVFGK::UnaryOp:
    case SVFGK::Phi:
    case SVFGK::IntraPhi:
    case SVFGK::InterPhi:
    case SVFGK::FormalParm:
    case SVFGK::ActualParm:
    case SVFGK::VarArg:
    case SVFGK::FormalRet:
    case SVFGK::ActualRet:
    case SVFGK::NullPtr:
      backtraceAlongDirectVF(pts, dpm);
      break;
    case SVFGK::Gep: {
      CPtSet gepPts;
      backtraceAlongDirectVF(gepPts, dpm);
      CPtSet filtered =
          derived().processGepPts(llvm::cast<GepSVFGNode>(node), gepPts);
      derived().unionDDAPts(pts, filtered);
      break;
    }
    case SVFGK::Load: {
      const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(node);
      if (!load->getValue() || !load->getValue()->getType()->isPointerTy())
        break;
      CPtSet loadPts;
      startNewPTCompFromLoadSrc(loadPts, dpm);
      derived().forEachElementInCPtSet(
          loadPts, [&](const CVar &obj, uint32_t /*objId*/) {
            DPIm objDpm = derived().getDPImWithOldCond(dpm, obj, load);
            backtraceAlongIndirectVF(pts, objDpm, CPtSet{});
          });
      break;
    }
    case SVFGK::Store: {
      const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(node);
      if (const llvm::StoreInst *si =
              llvm::dyn_cast_or_null<llvm::StoreInst>(store->getValue())) {
        if (!si->getValueOperand()->getType()->isPointerTy())
          break;
      } else {
        break;
      }
      if (derived().hasLoadDpm(dpm) &&
          derived().isMustAlias(derived().getLoadDpm(dpm), dpm)) {
        if (ddaStat_)
          ddaStat_->numOfMustAliases++;
        backtraceToStoreSrc(pts, dpm);
        break;
      }
      CPtSet storePts;
      startNewPTCompFromStoreDst(storePts, dpm);
      // Bug 1/5 fix (Store handler): getLoadCVar(dpm) is only valid when dpm
      // has an associated load DPM. If there is no load DPM (e.g. the solver
      // was started directly at a Store node, not via a Load), we cannot
      // determine which load object to match against. In that case,
      // conservatively propagate along the indirect value-flow edge (same as
      // the "else" branch).
      if (!derived().hasLoadDpm(dpm)) {
        backtraceAlongIndirectVF(pts, dpm, CPtSet{});
        break;
      }
      derived().forEachElementInCPtSet(storePts, [&](const CVar &storeObj,
                                                     uint32_t /*objId*/) {
        if (derived().propagateViaObj(storeObj, derived().getLoadCVar(dpm))) {
          DPIm objDpm = derived().getDPImWithOldCond(dpm, storeObj, store);
          backtraceToStoreSrc(pts, objDpm);
          if (derived().isStrongUpdate(storePts, store)) {
            if (ddaStat_) {
              ddaStat_->numOfStrongUpdates++;
              ddaStat_->strongUpdateStores.insert(store->getId());
            }
          } else {
            backtraceAlongIndirectVF(pts, objDpm, CPtSet{});
          }
        } else {
          backtraceAlongIndirectVF(pts, dpm, CPtSet{});
        }
      });
      break;
    }
    default:
      if (node->isMemNode())
        backtraceAlongIndirectVF(pts, dpm, CPtSet{});
      break;
    }
  }

  void backtraceAlongDirectVF(CPtSet &pts, const DPIm &oldDpm) {
    const SVFGNode *node = oldDpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!derived().isDirectEdge(edge))
        continue;
      SVFGNode *src = edge->getSrcNode();
      const uint32_t topLevelId = derived().getTopLevelValueId(src);
      if (topLevelId != 0)
        backwardPropDpm(pts, topLevelId, oldDpm, edge);
    }
  }

  void backtraceAlongIndirectVF(CPtSet &pts, const DPIm &oldDpm,
                                const CPtSet &curObjPts) {
    (void)curObjPts;
    const SVFGNode *node = oldDpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    uint32_t obj = oldDpm.getCurNodeID();
    if (obj == 0)
      return;
    if (svfg->isConstantObject(obj))
      return;
    // Match SVF DDAVFSolver behavior: skip constant objects when traversing
    // indirect value-flow edges.
    if (const llvm::Value *objVal = svfg->getObjectValue(obj)) {
      // Non-global constants are immutable objects (e.g., constant
      // expressions).
      if (llvm::isa<llvm::Constant>(objVal) &&
          !llvm::isa<llvm::GlobalValue>(objVal))
        return;
      // Constant globals are also immutable; skip indirect memory propagation.
      if (const auto *gv = llvm::dyn_cast<llvm::GlobalVariable>(objVal)) {
        if (gv->isConstant())
          return;
      }
    }
    for (SVFGEdge *edge : node->getInEdges()) {
      if (!derived().isIndirectEdge(edge))
        continue;
      const std::set<uint32_t> &guard = edge->getPointsTo();
      // Edge guard is object-sensitive. If the current object is not listed,
      // only keep the path when a wildcard/unknown object is present.
      if (!guard.empty() && guard.count(obj) == 0) {
        bool hasWildcard = false;
        for (uint32_t id : guard) {
          if (svfg->isUnknownObject(id)) {
            hasWildcard = true;
            break;
          }
        }
        if (!hasWildcard)
          continue;
      }
      backwardPropDpm(pts, oldDpm.getCurNodeID(), oldDpm, edge);
    }
  }

  void backwardPropDpm(CPtSet &pts, uint32_t ptrNodeId, const DPIm &oldDpm,
                       SVFGEdge *edge) {
    SVFGNode *src = edge->getSrcNode();
    if (!src)
      return;
    DPIm dpm(oldDpm);
    derived().setDpmLocVar(dpm, src, ptrNodeId);
    if (!derived().handleBKCondition(dpm, edge)) {
      if (ddaStat_)
        ddaStat_->numOfInfeasiblePath++;
      return;
    }
    // Bug 1 fix: only forward load-DPM info when oldDpm actually has one.
    // The old code called getLoadDpm/getLoadCVar unconditionally on every
    // indirect edge, which triggered assert(false) + unsound fallback when
    // oldDpm was a fresh DPItem (e.g. starting at EntryChi or FormalIn).
    if (derived().isIndirectEdge(edge) && derived().hasLoadDpm(oldDpm))
      derived().addLoadDpmAndCVar(dpm, derived().getLoadDpm(oldDpm),
                                  derived().getLoadCVar(oldDpm));
    if (ddaStat_)
      ddaStat_->numOfDPM++;
    derived().unionDDAPts(pts, findPT(dpm));
  }

  void startNewPTCompFromLoadSrc(CPtSet &loadPts, const DPIm &oldDpm) {
    const LoadSVFGNode *load = llvm::cast<LoadSVFGNode>(oldDpm.getLoc());
    SVFG *svfg = derived().getSVFG();
    if (!svfg)
      return;
    const llvm::LoadInst *loadInst =
        llvm::dyn_cast_or_null<llvm::LoadInst>(load->getValue());
    if (!loadInst)
      return;
    SVFGNode *loadSrc = derived().getDefNodeForValue(loadInst->getPointerOperand());
    if (!loadSrc)
      return;
    const uint32_t ptrNodeId = derived().getTopLevelValueId(loadSrc);
    if (ptrNodeId == 0)
      return;
    // Bug 2 fix: the SVFG builder connects the load-pointer operand to the
    // LoadSVFGNode with IntraCopy (not IntraDirect). Using IntraDirect caused
    // getIntraVFGEdge to always return nullptr, silently dropping the load's
    // pointer source and returning an empty points-to set for every load.
    // Try IntraCopy first; fall back to IntraDirect for compatibility with
    // alternative builders that may use IntraDirect.
    SVFGEdge *edge = svfg->getIntraVFGEdge(loadSrc, load, SVFGEdgeK::IntraCopy);
    if (!edge)
      edge = svfg->getIntraVFGEdge(loadSrc, load, SVFGEdgeK::IntraDirect);
    if (edge)
      backwardPropDpm(loadPts, ptrNodeId, oldDpm, edge);
  }

  void startNewPTCompFromStoreDst(CPtSet &storePts, const DPIm &oldDpm) {
    const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(oldDpm.getLoc());
    SVFG *svfg = derived().getSVFG();
    if (!svfg)
      return;
    const llvm::StoreInst *storeInst =
        llvm::dyn_cast_or_null<llvm::StoreInst>(store->getValue());
    if (!storeInst)
      return;
    SVFGNode *storeDst =
        derived().getDefNodeForValue(storeInst->getPointerOperand());
    if (!storeDst)
      return;
    const uint32_t ptrNodeId = derived().getTopLevelValueId(storeDst);
    if (ptrNodeId == 0)
      return;
    // Bug 2 fix (store side): same as load — try IntraCopy first.
    SVFGEdge *edge =
        svfg->getIntraVFGEdge(storeDst, store, SVFGEdgeK::IntraCopy);
    if (!edge)
      edge = svfg->getIntraVFGEdge(storeDst, store, SVFGEdgeK::IntraDirect);
    if (edge)
      backwardPropDpm(storePts, ptrNodeId, oldDpm, edge);
  }

  void backtraceToStoreSrc(CPtSet &pts, const DPIm &oldDpm) {
    const StoreSVFGNode *store = llvm::cast<StoreSVFGNode>(oldDpm.getLoc());
    const llvm::Value *valueOperand =
        llvm::cast<llvm::StoreInst>(store->getValue())->getValueOperand();
    SVFGNode *storeSrc = derived().getDefNodeForValue(valueOperand);
    if (!storeSrc)
      return;
    const uint32_t storeSrcId = derived().getTopLevelValueId(storeSrc);
    if (storeSrcId == 0)
      return;
    SVFG *svfg = derived().getSVFG();
    if (!svfg)
      return;
    SVFGEdge *edge =
        svfg->getIntraVFGEdge(storeSrc, store, SVFGEdgeK::IntraDirect);
    if (!edge)
      return;
    backwardPropDpm(pts, storeSrcId, oldDpm, edge);
  }

  /// Re-evaluate dependents when `dpm` got new points-to facts.
  ///
  /// This is the demand-driven refinement loop: when a query discovers new
  /// facts, already-seen dependent DPMs are invalidated and recomputed.
  void reCompute(const DPIm &dpm) {
    const SVFGNode *node = dpm.getLoc();
    SVFG *svfg = derived().getSVFG();
    if (!node || !svfg)
      return;
    const auto &indCallSites = svfg->getIndCallSites(dpm.getCurNodeID());
    if (!indCallSites.empty() && derived().getSVFGBuilder()) {
      const CPtSet &funPts = getCachedPointsTo(dpm);
      std::vector<SVFGEdge *> newEdges;
      derived().connectIndirectCallees(dpm, funPts, newEdges);
      if (!newEdges.empty()) {
        derived().onIndirectEdgesAdded();
        reComputeForEdges(dpm, newEdges, true);
      }
    }
    const std::vector<SVFGEdge *> &edgeSet = node->getOutEdges();
    reComputeForEdges(dpm, edgeSet, false);
  }

  void reComputeForEdges(const DPIm &dpm,
                         const std::vector<SVFGEdge *> &edgeSet,
                         bool indirectCall) {
    for (SVFGEdge *edge : edgeSet) {
      SVFGNode *dst = edge->getDstNode();
      if (!dst)
        continue;
      auto it = locToDpmSetMap_.find(dst->getId());
      if (it == locToDpmSetMap_.end())
        continue;
      for (const DPIm &dstDpm : it->second) {
        // Bug 3 fix: the old condition
        //   !indirectCall && isIndirectEdge(edge) && !isa<LoadSVFGNode>(dst)
        // only re-evaluated dstDpm when dstDpm.getCurNodeID() ==
        // dpm.getCurNodeID(). For memory SSA nodes (StoreChiSVFGNode,
        // IntraMSSAPhiSVFGNode, FormalInSVFGNode, etc.) the object IDs
        // typically differ, so the condition was never true and new points-to
        // facts were never propagated forward through those nodes.
        //
        // The correct rule (matching SVF DDAVFSolver::reComputeForEdges) is:
        //   - For indirect edges to non-Load memory nodes: re-evaluate only
        //     when the object IDs match (same object flowing through the edge).
        //   - For all other cases (direct edges, indirect call edges, indirect
        //     edges to Load nodes): always re-evaluate.
        //
        // We additionally check the edge guard: if the guard is non-empty and
        // does not contain dpm.getCurNodeID() (and has no wildcard), the edge
        // cannot carry the current object, so skip it.
        if (!indirectCall && derived().isIndirectEdge(edge) &&
            !llvm::isa<LoadSVFGNode>(dst)) {
          // Check edge guard before deciding whether to re-evaluate.
          const std::set<uint32_t> &guard = edge->getPointsTo();
          bool guardAllows = guard.empty();
          if (!guardAllows) {
            if (guard.count(dpm.getCurNodeID()))
              guardAllows = true;
            if (!guardAllows) {
              SVFG *svfg = derived().getSVFG();
              for (uint32_t id : guard) {
                if (svfg && svfg->isUnknownObject(id)) {
                  guardAllows = true;
                  break;
                }
              }
            }
          }
          if (!guardAllows)
            continue;
          // Re-evaluate if the object IDs match OR if the destination is a
          // memory node that may merge multiple definitions (PHI, FormalIn).
          const bool isMemMergeNode = llvm::isa<IntraMSSAPhiSVFGNode>(dst) ||
                                      llvm::isa<FormalInSVFGNode>(dst) ||
                                      dst->getNodeKind() == SVFGK::EntryChi;
          if (dstDpm.getCurNodeID() == dpm.getCurNodeID() || isMemMergeNode) {
            if (ddaStat_)
              ddaStat_->numOfStepInCycle++;
            clearbkVisited(dstDpm);
            findPT(dstDpm);
          }
        } else {
          if (ddaStat_)
            ddaStat_->numOfStepInCycle++;
          clearbkVisited(dstDpm);
          findPT(dstDpm);
        }
      }
    }
  }

  void markbkVisited(const DPIm &dpm) { backwardVisited_.insert(dpm); }
  void clearbkVisited(const DPIm &dpm) { backwardVisited_.erase(dpm); }
  bool isbkVisited(const DPIm &dpm) const {
    return backwardVisited_.count(dpm) != 0;
  }

  const CPtSet &getCachedPointsTo(const DPIm &dpm) const {
    const auto &cache = derived().isTopLevelPtrStmt(dpm.getLoc())
                            ? dpmToTLPtsMap_
                            : dpmToADPtsMap_;
    auto it = cache.find(dpm);
    return (it != cache.end()) ? it->second : derived().getEmptyCPtSetRef();
  }

  void updateCachedPointsTo(const DPIm &dpm, const CPtSet &pts) {
    if (derived().unionDDAPts(dpm, pts))
      reCompute(dpm);
  }

  void addDpmToLoc(const DPIm &dpm) {
    const SVFGNode *loc = dpm.getLoc();
    if (loc)
      locToDpmSetMap_[loc->getId()].insert(dpm);
  }

  bool testOutOfBudget(const DPIm &dpm) {
    if (outOfBudget_)
      return true;
    if (ddaStat_)
      ddaStat_->numOfStep++;
    numSteps_++;
    if (numSteps_ > derived().getMaxBudget()) {
      outOfBudget_ = true;
      return true;
    }
    return derived().isOutOfBudgetDpm(dpm);
  }

  void addOutOfBudgetDpm(const DPIm &dpm) {
    derived().insertOutOfBudgetDpm(dpm);
  }

  void OOBResetVisited() {
    for (const auto &p : locToDpmSetMap_) {
      for (const DPIm &dpm : p.second) {
        if (!derived().isOutOfBudgetDpm(dpm))
          clearbkVisited(dpm);
      }
    }
  }

  /// Per-query DFS visited set (for termination on value-flow cycles).
  std::set<DPIm> backwardVisited_;
  /// Cache for top-level pointer statements (TLP cache in SVF terminology).
  std::map<DPIm, CPtSet> dpmToTLPtsMap_;
  /// Cache for address-taken/memory statements.
  std::map<DPIm, CPtSet> dpmToADPtsMap_;
  /// Reverse dependency index: SVFG node -> DPMs currently waiting on it.
  std::map<uint32_t, std::set<DPIm>> locToDpmSetMap_;
  DDAStat *ddaStat_;
  /// Number of transfer steps consumed in the current query.
  uint32_t numSteps_;
  /// Sticky flag once the current query exceeds budget.
  bool outOfBudget_;
};

} // namespace analysis
} // namespace lotus
