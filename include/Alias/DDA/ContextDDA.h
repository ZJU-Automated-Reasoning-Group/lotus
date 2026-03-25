//===- ContextDDA.h -- Context-sensitive DDA (SVF-style) ------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// ContextDDA: flow-sensitive, context-sensitive demand-driven pointer analysis.
// Uses CxtLocDPItem (call-string context) and CxtPtSet. Shares DDAVFSolver
// template with FlowDDA. On out-of-budget downgrades to FlowDDA.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/CxtDPItem.h"
#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/DDAVFSolver.h"
#include "Alias/DDA/FlowDDA.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Value.h>

namespace llvm {
class Module;
} // namespace llvm

namespace lotus {
namespace analysis {

/// Flow-sensitive, context-sensitive DDA. Uses DDAVFSolver and call-string
/// context in handleBKCondition.
class ContextDDA
    : public DDAVFSolver<CxtVar, CxtPtSet, CxtLocDPItem, ContextDDA> {
  template <typename CVar, typename CPtSet, typename DPIm, typename D>
  friend class DDAVFSolver;

public:
  explicit ContextDDA(FlowDDA *flowDDA, DDAClient *client);
  ~ContextDDA();

  bool run(llvm::Module &M);
  void setClient(DDAClient *client) { client_ = client; }
  DDAClient *getClient() const { return client_; }
  FlowDDA *getFlowDDA() const { return flowDDA_; }
  SVFG *getSVFG() const { return flowDDA_ ? flowDDA_->getSVFG() : nullptr; }
  SVFGBuilder *getSVFGBuilder() const {
    return flowDDA_ ? flowDDA_->getSVFGBuilder() : nullptr;
  }

  /// Compute context-sensitive points-to for pointer value (empty context).
  CxtPtSet computeDDAPts(const llvm::Value *ptr);
  /// Compute context-sensitive points-to for (context, node id).
  const CxtPtSet &computeDDAPts(const CxtVar &cxtVar);
  /// Run DDA for each candidate from the client.
  void answerQueries();
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);

  /// Handle call-string on call/ret edges; return false to prune.
  bool handleBKCondition(CxtLocDPItem &dpm, SVFGEdge *edge);
  void handleOutOfBudgetDpm(const CxtLocDPItem &dpm);

  /// Call site ID from edge (from getCallSite()); 0 if none.
  uint32_t getCSIDAtCall(CxtLocDPItem &dpm, SVFGEdge *edge);
  uint32_t getCSIDAtRet(CxtLocDPItem &dpm, SVFGEdge *edge);

  /// True if call site csId is in a recursive SCC (Tarjan on module call
  /// graph).
  bool isEdgeInRecursion(uint32_t csId) const;
  /// Pop recursive call sites from dpm's context until top is not in recursion.
  void popRecursiveCallSites(CxtLocDPItem &dpm);

  /// Initialize context-insensitive edges (call/ret in recursion or SVFG SCC).
  /// Call after run(M) so recursion info and SVFG are available.
  void initInsensitiveEdges();
  void setInsensitiveRecursion(bool enable) {
    insensitiveRecursion_ = enable;
  }
  void setInsensitiveCycle(bool enable) { insensitiveCycle_ = enable; }
  bool getInsensitiveRecursion() const { return insensitiveRecursion_; }
  bool getInsensitiveCycle() const { return insensitiveCycle_; }
  const std::unordered_set<const SVFGEdge *> &getInsensitiveEdgeSet() const {
    return insensitveEdges_;
  }

  static void setMaxCxtLen(uint32_t max) { ContextCond::setMaxCxtLen(max); }
  static void setMaxPathLen(uint32_t max) { ContextCond::setMaxPathLen(max); }

  /// Context compatibility for context-sensitive propagation (SVF
  /// isCondCompatible).
  bool isCondCompatible(const ContextCond &cxt1, const ContextCond &cxt2,
                        bool singleton) const;

private:
  void buildRecursionInfo();

  /// DDAVFSolver interface (CRTP).
  SVFGNode *getDefNodeForValue(const llvm::Value *v) const;
  uint32_t getTopLevelValueId(const SVFGNode *node) const;
  SVFGNodeBS getObjectIdsForValue(const llvm::Value *v) const;
  static bool isDirectEdge(SVFGEdge *e);
  static bool isIndirectEdge(SVFGEdge *e);
  CxtPtSet getConservativeCPts(const CxtLocDPItem &dpm) const;
  void handleAddr(CxtPtSet &pts, const CxtLocDPItem &dpm,
                  const AddrSVFGNode *addr);
  CxtPtSet processGepPts(const GepSVFGNode *gep, const CxtPtSet &srcPts);

protected:
  bool isStrongUpdate(const CxtPtSet &dstPts, const StoreSVFGNode *store);

private:
  uint32_t getPtrNodeID(uint32_t var) const { return var; }
  void addDDAPts(CxtPtSet &pts, uint32_t var);
  void unionDDAPts(CxtPtSet &target, const CxtPtSet &source);
  bool unionDDAPts(const CxtLocDPItem &dpm, const CxtPtSet &pts);
  CxtLocDPItem getDPImWithOldCond(const CxtLocDPItem &oldDpm, const CxtVar &var,
                                  const SVFGNode *loc) const;
  void resolveFunPtr(const CxtLocDPItem &dpm);
  bool isTopLevelPtrStmt(const SVFGNode *stmt) const;
  void setDpmLocVar(CxtLocDPItem &dpm, SVFGNode *src, uint32_t ptrNodeId);
  void addLoadDpmAndCVar(const CxtLocDPItem &dpm, const CxtLocDPItem &loadDpm,
                         const CxtVar &loadCVar);
  bool hasLoadDpm(const CxtLocDPItem &dpm) const;
  CxtLocDPItem getLoadDpm(const CxtLocDPItem &dpm) const;
  CxtVar getLoadCVar(const CxtLocDPItem &dpm) const;
  bool isMustAlias(const CxtLocDPItem &loadDpm,
                   const CxtLocDPItem &storeDpm) const;
  bool propagateViaObj(const CxtVar &storeObj, const CxtVar &loadObj) const;
  void forEachObjId(const CxtPtSet &pts,
                    std::function<void(uint32_t)> callback) const;
  void forEachElementInCPtSet(
      const CxtPtSet &pts,
      std::function<void(const CxtVar &, uint32_t)> callback) const;
  const CxtPtSet &getEmptyCPtSetRef() const;
  void connectIndirectCallees(const CxtLocDPItem &dpm, const CxtPtSet &funPts,
                              std::vector<SVFGEdge *> &newEdges);
  void onIndirectEdgesAdded() {
    buildRecursionInfo();
    initInsensitiveEdges();
  }
  void resetQueryLoadMaps();
  void insertOutOfBudgetDpm(const CxtLocDPItem &dpm);
  bool isOutOfBudgetDpm(const CxtLocDPItem &dpm) const;
  uint32_t getMaxBudget() const { return DPItem::getMaxBudget(); }

  FlowDDA *flowDDA_;
  DDAClient *client_;
  std::map<CxtLocDPItem, CxtLocDPItem> dpmToLoadDpmMap_;
  std::map<CxtLocDPItem, CxtVar> dpmToLoadCVarMap_;
  std::set<CxtLocDPItem> outOfBudgetDpms_;
  std::unordered_set<uint32_t> recursiveCallSiteIds_;
  /// Call/ret edges treated context-insensitively (recursion or value-flow
  /// cycle).
  std::unordered_set<const SVFGEdge *> insensitveEdges_;
  bool insensitiveRecursion_ = false;
  bool insensitiveCycle_ = false;
};

} // namespace analysis
} // namespace lotus
