//===- FlowDDA.h -- Flow-sensitive demand-driven pointer analysis ---------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//
//
// FlowDDA: Flow-Sensitive Demand-Driven Alias Analysis
//
// This file implements demand-driven pointer analysis using value-flow graphs,
// following SVF's FlowDDA design (FSE'16, TSE'18).
//
// == What is Demand-Driven Analysis? ==
//
// Unlike exhaustive pointer analysis that computes points-to sets for all
// pointers upfront, demand-driven analysis (DDA) computes results on-demand:
// - Only analyzes pointers when queried
// - Backtracks through value-flow graph to find definitions
// - Caches results to avoid recomputation
// - Falls back to conservative analysis when budget exceeded
//
// == Analysis Characteristics ==
//
// - Flow-sensitive: Distinguishes different program points
// - Context-insensitive: Merges all calling contexts (see ContextDDA for
// context-sensitive)
// - Field-sensitive: Tracks individual struct fields
// - On-demand: Computes points-to sets only when queried
//
// == Algorithm Overview ==
//
// 1. Query: getPointsTo(ptr) - compute points-to set for a pointer
// 2. Backward traversal: Follow SVFG edges backward from ptr's definition
// 3. Statement handling:
//    - Addr: Add allocation object to points-to set
//    - Copy/Phi: Continue backward through operands
//    - Load: Get points-to of pointer, then indirect backward traversal
//    - Store: Apply strong/weak update based on must-alias
//    - GEP: Adjust field offsets in points-to set
// 4. Caching: Store computed points-to sets to avoid recomputation
// 5. Budget: Limit traversal steps; fallback to conservative PTA if exceeded
//
// == Example ==
//
// ```c
// int x, y;
// int *p = &x;        // p -> {x}
// if (cond)
//   p = &y;           // p -> {x, y} (flow-sensitive merge)
// int z = *p;         // Query: what does p point to here?
// ```
//
// DDA backward traversal:
// 1. Start at use of p in `*p`
// 2. Find phi node merging p from both branches
// 3. Backward through both branches:
//    - Branch 1: p = &x → add x to points-to set
//    - Branch 2: p = &y → add y to points-to set
// 4. Result: p -> {x, y}
//
// == References ==
//
// - "On-Demand Strong Update Analysis via Value-Flow Refinement"
//   Yulei Sui, Jingling Xue. FSE 2016.
// - "Detecting Memory Leaks Statically with Full-Sparse Value-Flow Analysis"
//   Yulei Sui, Ding Ye, Jingling Xue. TSE 2014.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "Alias/DDA/DDAVFSolver.h"
#include "Alias/DDA/DPItem.h"
#include "IR/ICFG/ICFG.h"

namespace lotus {
namespace analysis {
class DDAClient;
class DDAStat;
} // namespace analysis
} // namespace lotus
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/Value.h>

namespace llvm {
class Module;
class Function;
class Instruction;
class LoopInfo;
} // namespace llvm

namespace lotus {
namespace analysis {

using LocDPItem = StmtDPItem<SVFGNode>;

/// FlowDDA: Flow-sensitive, context-insensitive demand-driven pointer analysis.
///
/// This class implements on-demand pointer analysis using backward traversal
/// through the Sparse Value-Flow Graph (SVFG). It computes points-to sets only
/// when queried, making it efficient for query-driven tools.
///
/// Key methods:
/// - getPointsTo(ptr): Compute points-to set for a pointer value
/// - mayAlias(v1, v2): Check if two pointers may alias
/// - run(module): Initialize analysis on an LLVM module
///
/// Usage example:
/// ```cpp
/// FlowDDA dda;
/// dda.run(module);
/// auto pts = dda.getPointsTo(ptr);
/// for (uint32_t objId : pts) {
///   // Process pointed-to object
/// }
/// ```
///
/// Budget control:
/// - setDefaultMaxBudget(n): Limit traversal steps per query
/// - When budget exceeded, falls back to conservative PTA results
///
/// Client support:
/// - setClient(client): Set DDAClient for candidate query collection
/// - answerQueries(): Run DDA for all client candidates
class FlowDDA : public DDAVFSolver<uint32_t, std::unordered_set<uint32_t>,
                                   LocDPItem, FlowDDA> {
  template <typename CVar, typename CPtSet, typename DPIm, typename D>
  friend class DDAVFSolver;

public:
  using PtsSet = std::unordered_set<uint32_t>;

  FlowDDA();
  virtual ~FlowDDA();

  FlowDDA(const FlowDDA &) = delete;
  FlowDDA &operator=(const FlowDDA &) = delete;

  bool run(llvm::Module &M);

  PtsSet getPointsTo(const llvm::Value *ptr);
  bool getPointsToSet(const llvm::Value *ptr,
                      std::vector<const llvm::Value *> &out);
  bool mayAlias(const llvm::Value *v1, const llvm::Value *v2);
  bool mayNull(const llvm::Value *ptr);

  SVFG *getSVFG() const { return svfg_.get(); }
  const SVFG *getSVFGConst() const { return svfg_.get(); }
  SVFGBuilder *getSVFGBuilder() const { return svfgBuilder_.get(); }
  const llvm::Module *getModule() const { return module_; }
  bool isInitialized() const { return initialized_; }

  /// Max steps per query (out-of-budget then fallback to conservative PTA).
  static void setDefaultMaxBudget(uint32_t budget) {
    defaultMaxBudget_ = budget;
  }
  static uint32_t getDefaultMaxBudget() { return defaultMaxBudget_; }

  /// Client for candidate queries and callbacks (SVF-style).
  void setClient(DDAClient *client) { client_ = client; }
  DDAClient *getClient() const { return client_; }
  /// Run DDA for each candidate from the client; no-op if no client.
  void answerQueries();
  /// FlowDDA: no context check; returns true. Override in ContextDDA.
  virtual bool handleBKCondition(LocDPItem &dpm, SVFGEdge *edge);
  /// Called when a dpm hits step budget (optional downgrade / stats).
  virtual void handleOutOfBudgetDpm(const LocDPItem &dpm);
  /// Entry used by client: compute points-to for pointer value (same as
  /// getPointsTo).
  PtsSet computeDDAPts(const llvm::Value *ptr) { return getPointsTo(ptr); }

  DDAStat *getStat() const { return ddaStat_.get(); }
  /// @brief Query object IDs for a pointer/allocation value (PTA-backed).
  SVFGNodeBS getObjectIdsForValue(const llvm::Value *v) const;
  /// @brief Query if a function is recursive in the current module.
  bool isRecursiveFunction(const llvm::Function *f) const;
  bool isInLoop(const llvm::Instruction *inst) const;

protected:
  // DDAVFSolver interface (CRTP). getSVFG/getSVFGBuilder/handleBKCondition are
  // public above.
  SVFGNode *getDefNodeForValue(const llvm::Value *v) const;
  uint32_t getTopLevelValueId(const SVFGNode *node) const;
  static bool isDirectEdge(SVFGEdge *e);
  static bool isIndirectEdge(SVFGEdge *e);
  PtsSet getConservativeCPts(const LocDPItem &dpm) const;
  void handleAddr(PtsSet &pts, const LocDPItem &dpm, const AddrSVFGNode *addr);
  PtsSet processGepPts(const GepSVFGNode *gep, const PtsSet &srcPts);
  bool isStrongUpdate(const PtsSet &dstPts, const StoreSVFGNode *store);
  uint32_t getPtrNodeID(uint32_t var) const { return var; }
  void addDDAPts(PtsSet &pts, uint32_t var) { pts.insert(var); }
  void unionDDAPts(PtsSet &target, const PtsSet &source);
  bool unionDDAPts(const LocDPItem &dpm, const PtsSet &pts);
  LocDPItem getDPImWithOldCond(const LocDPItem &oldDpm, uint32_t objId,
                               const SVFGNode *loc) const;
  void resolveFunPtr(const LocDPItem &dpm);
  bool isTopLevelPtrStmt(const SVFGNode *stmt) const;
  bool hasLoadDpm(const LocDPItem &dpm) const;
  LocDPItem getLoadDpm(const LocDPItem &dpm) const;
  uint32_t getLoadCVar(const LocDPItem &dpm) const;
  bool isMustAlias(const LocDPItem &loadDpm, const LocDPItem &storeDpm) const;
  bool propagateViaObj(uint32_t storeObj, uint32_t loadObj) const;
  void forEachObjId(const PtsSet &pts,
                    std::function<void(uint32_t)> callback) const;
  void forEachElementInCPtSet(
      const PtsSet &pts,
      std::function<void(uint32_t, uint32_t)> callback) const;
  const PtsSet &getEmptyCPtSetRef() const;
  void setDpmLocVar(LocDPItem &dpm, SVFGNode *src, uint32_t ptrNodeId);
  void addLoadDpmAndCVar(const LocDPItem &dpm, const LocDPItem &loadDpm,
                         uint32_t loadCVarObjId);
  void connectIndirectCallees(const LocDPItem &dpm, const PtsSet &funPts,
                              std::vector<SVFGEdge *> &newEdges);
  void onIndirectEdgesAdded() {
    // The SVFG has been mutated (new call edges added). Invalidate ptsCache_
    // so that subsequent mayAlias/mayNull calls recompute against the updated
    // graph rather than returning stale pre-mutation results (bug #9).
    ptsCache_.clear();
    buildRecursionInfo();
  }
  void resetQueryLoadMaps();
  void insertOutOfBudgetDpm(const LocDPItem &dpm);
  bool isOutOfBudgetDpm(const LocDPItem &dpm) const;
  uint32_t getMaxBudget() const { return LocDPItem::getMaxBudget(); }

private:
  /// Strong-update refinements: exclude heap, array, recursion (best-effort).
  bool isHeapCondMemObj(uint32_t objId, const StoreSVFGNode *store) const;
  bool isArrayCondMemObj(uint32_t objId) const;
  bool isFieldInsenCondMemObj(uint32_t objId) const;
  bool isLocalCVarInRecursion(uint32_t objId) const;

  PtsSet getPointsToCached(const llvm::Value *ptr);
  void buildRecursionInfo();
  void buildLoopInfo();

  std::unique_ptr<::ICFG> icfg_;
  std::unique_ptr<::ICFGBuilder> icfgBuilder_;
  std::unique_ptr<SVFGBuilder> svfgBuilder_;
  std::unique_ptr<SVFG> svfg_;

  std::unordered_map<const llvm::Value *, PtsSet> ptsCache_;
  std::map<LocDPItem, LocDPItem> dpmToLoadDpmMap_;
  std::map<LocDPItem, uint32_t> dpmToLoadCVarMap_;
  std::set<LocDPItem> outOfBudgetDpms_;
  bool initialized_ = false;
  DDAClient *client_ = nullptr;
  const llvm::Module *module_ = nullptr;
  std::unique_ptr<DDAStat> ddaStat_;
  std::unordered_set<const llvm::Function *> recursiveFunctions_;
  std::unordered_map<const llvm::Function *, std::unique_ptr<llvm::LoopInfo>>
      loopInfoMap_;
  static uint32_t defaultMaxBudget_;
};

/// Backward-compatibility alias for code that still refers to DemandDrivenAA.
using DemandDrivenAA = FlowDDA;

} // namespace analysis
} // namespace lotus
