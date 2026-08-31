//===- SVFG.h -- Sparse Value-Flow Graph
//--------------------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// SVFG: Sparse Value-Flow Graph for Interprocedural Program Analysis
//
// The Sparse Value-Flow Graph (SVFG) is a program representation that captures
// def-use chains for both register values and memory locations across function
// boundaries. It extends traditional SSA form to handle memory operations and
// interprocedural control flow.
//
// == What is SVFG? ==
//
// SVFG represents value flow in a program as a directed graph where:
// - Nodes represent definitions (assignments, loads, stores, parameters)
// - Edges represent value-flow relationships (def-use chains)
// - Memory SSA form tracks memory versions through MU/CHI nodes
//
// Example:
//   int x = 5;           // AddrNode (defines x)
//   int *p = &x;         // CopyNode (p = &x)
//   int *y = *p;         // LoadNode reads a reaching memory version
//   *p = 10;             // StoreNode defines the next memory version
//
// == Key Features ==
//
// 1. Sparse Representation:
//    - Only tracks values that flow through memory or across functions
//    - Omits purely local computations (e.g., x = a + b stays in ICFG)
//    - Reduces graph size while preserving essential def-use information
//
// 2. Memory SSA:
//    - Extends SSA to memory locations using MU (use) and CHI (def) nodes
//    - Each memory region has versioned definitions (e.g., mem_1, mem_2)
//    - Enables precise tracking of memory dependencies
//
// 3. Interprocedural Value Flow:
//    - FormalIn/FormalOut: Parameter/return value definitions in callee
//    - ActualIn/ActualOut: Argument/return value uses at call site
//    - Call/Return edges connect caller and callee contexts
//
// 4. Pointer Analysis Integration:
//    - Uses AserPTA for points-to information
//    - Guards memory edges with points-to sets (which objects may be accessed)
//    - Supports field-sensitive and context-sensitive analysis
//
// == Use Cases ==
//
// - Demand-Driven Alias Analysis (DDA): Query alias relationships on-demand
// - Program Slicing: Extract program fragments affecting specific values
// - Taint Analysis: Track information flow from sources to sinks
// - Bug Detection: Find use-after-free, null dereferences, buffer overflows
// - Optimization: Identify dead stores, redundant loads
//
// == DDA-Facing Contract ==
//
// For demand-driven analyses (DDA), SVFG provides:
// - Object IDs in edge guards are abstract memory objects (not node IDs)
// - `getObjectValue(objId)` maps object IDs to LLVM values
// - `getObjectInfo(objId)` provides metadata (constant, unknown, function,
// etc.)
// - Indirect call site indices support on-the-fly call edge materialization
// - Conservative unknown wildcard object for unresolved pointers
//
// == References ==
//
// Based on SVF (Static Value-Flow Analysis Framework):
// - "Sparse Flow-Sensitive Pointer Analysis for Multithreaded Programs"
//   Yulei Sui, Jingling Xue. CGO 2016.
// - "SVF: Interprocedural Static Value-Flow Analysis in LLVM"
//   Yulei Sui, Ding Ye, Jingling Xue. CC 2016.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus {
namespace analysis {

class SVFG;

/// @brief SVFG node ID to node map
using SVFGNodeMap = std::unordered_map<uint32_t, SVFGNode *>;

/// @brief SVFG node set
using SVFGNodeSet = std::set<SVFGNode *>;

/// @brief LLVM instruction to SVFG node ID map for definition sites
using InstToDefMap = std::unordered_map<const llvm::Instruction *, uint32_t>;

/// @brief Value to SVFG node ID map (for top-level pointers)
using ValueToNodeMap = std::unordered_map<const llvm::Value *, uint32_t>;

/// @brief Memory region version to SVFG node ID map
struct MemSSAKey {
  uint32_t memReg = 0;
  uint32_t version = 0;

  bool operator==(const MemSSAKey &other) const {
    return memReg == other.memReg && version == other.version;
  }
};

struct MemSSAKeyHash {
  size_t operator()(const MemSSAKey &key) const noexcept {
    return std::hash<uint32_t>()(key.memReg) ^
           (std::hash<uint32_t>()(key.version) << 1);
  }
};

using MRVerToNodeMap = std::unordered_map<MemSSAKey, SVFGNode *, MemSSAKeyHash>;

/// @brief Callsite instruction to actual-in/out SVFG node set map
using CallSiteToActualMap =
    std::unordered_map<const llvm::CallBase *, SVFGNodeSet>;

/// @brief Callsite instruction to actual parameter/return SVFG node set map
using CallSiteToActualParmMap =
    std::unordered_map<const llvm::CallBase *, SVFGNodeSet>;
using CallSiteToActualRetMap =
    std::unordered_map<const llvm::CallBase *, SVFGNodeSet>;

/// @brief Function to formal-in/out SVFG node set map
using FuncToFormalMap = std::unordered_map<const llvm::Function *, SVFGNodeSet>;

/// @brief Function to formal parameter/return SVFG node set map
using FuncToFormalParmMap =
    std::unordered_map<const llvm::Function *, SVFGNodeSet>;
using FuncToFormalRetMap =
    std::unordered_map<const llvm::Function *, SVFGNodeSet>;

/// @brief Legacy import index for LoadMu nodes (V6 text format only)
using LoadInstToMuMap = std::unordered_map<const llvm::LoadInst *, SVFGNodeSet>;

/// @brief Legacy import index for StoreChi nodes (V6 text format only)
using StoreInstToChiMap =
    std::unordered_map<const llvm::StoreInst *, SVFGNodeSet>;

/// @brief SVFG statistics
struct SVFGStat {
  uint32_t numNodes = 0;
  uint32_t numEdges = 0;
  uint32_t numAddrNodes = 0;
  uint32_t numCopyNodes = 0;
  uint32_t numLoadNodes = 0;
  uint32_t numStoreNodes = 0;
  uint32_t numGepNodes = 0;
  uint32_t numPhiNodes = 0;
  uint32_t numMemNodes = 0;
  uint32_t numParamNodes = 0;
  uint32_t numCallEdges = 0;
  uint32_t numRetEdges = 0;
  uint32_t numIntraEdges = 0;
};

/// @brief Main SVFG class
class SVFG {
public:
  /// @brief Iterator types
  using iterator = SVFGNodeMap::iterator;
  using const_iterator = SVFGNodeMap::const_iterator;

  /// @brief Object metadata for DDA (heap/stack/field-insensitive/etc.).
  ///
  /// This metadata is monotonic and merged by OR in updateObjectInfo().
  /// It is intentionally conservative: once an object is marked as unknown,
  /// constant, field-insensitive, etc., the bit remains set.
  struct ObjectInfo {
    bool isHeap = false;
    bool isConcreteHeap = false;
    bool isStack = false;
    bool isGlobal = false;
    bool isFunction = false;
    bool isConstant = false;
    bool isFieldInsensitive = false;
    bool isArray = false;
    bool isUnknown = false;
    uint32_t baseObjId = 0;
    bool isSingleton = false;
  };

private:
  /// @brief Primary node map: ID -> Node
  SVFGNodeMap nodeMap;

  /// @brief Definition mapping: ICFGNode -> SVFGNode ID
  InstToDefMap instToDefMap;

  /// @brief Value mapping: LLVM Value -> SVFGNode ID (top-level pointers)
  ValueToNodeMap valueToNodeMap;
  /// @brief Stable defining node per top-level value ID.
  ///
  /// Auxiliary nodes such as ActualParm/ActualRet may reuse an existing
  /// valueId, so DDA cannot recover the canonical defining node by scanning
  /// all SVFG nodes. This map records the defining node chosen by the
  /// builder's `setValueNode()` binding.
  std::unordered_map<uint32_t, uint32_t> valueIdToDefNodeMap;

  /// @brief Memory SSA mapping: Memory region version -> SVFGNode ID
  MRVerToNodeMap mssaVerToNodeMap;

  /// @brief Callsite mappings
  CallSiteToActualMap callSiteToActualInMap;  // ActualIn (memory)
  CallSiteToActualMap callSiteToActualOutMap; // ActualOut (memory)
  CallSiteToActualParmMap callSiteToActualParmMap;
  CallSiteToActualRetMap callSiteToActualRetMap;

  /// @brief Function mappings
  FuncToFormalMap funcToFormalInMap;  // FormalIn (memory)
  FuncToFormalMap funcToFormalOutMap; // FormalOut (memory)
  FuncToFormalParmMap funcToFormalParmMap;
  FuncToFormalRetMap funcToFormalRetMap;

  /// @brief Memory SSA instruction indices
  LoadInstToMuMap loadInstToMuMap;
  StoreInstToChiMap storeInstToChiMap;

  /// @brief Associated ICFG
  const ICFG *icfg;

  /// @brief Next available node ID
  uint32_t nextNodeId;

  /// @brief Statistics
  SVFGStat stat;

  /// @brief Debug labels for abstract objects used in points-to sets.
  ///
  /// Keys are object IDs (abstract objects), not SVFG node IDs.
  std::unordered_map<uint32_t, std::string> objectDebug;
  std::unordered_map<uint32_t, const llvm::Value *> objIdToValue;
  std::unordered_map<const llvm::Value *, uint32_t> valueToObjId;
  std::unordered_map<const llvm::Value *, SVFGNodeBS> valueToObjIds;
  std::unordered_map<uint32_t, ObjectInfo> objIdToInfo;
  std::unordered_map<uint32_t, std::string> nodeFunctionDebug;
  std::unordered_map<uint32_t, std::string> nodeCallSiteDebug;

  /// @brief Indirect-callsite indices used by DDA for on-the-fly call graph
  /// refinement.
  ///
  /// `funPtrToIndCallSites` indexes unresolved indirect callsites by the
  /// defining function-pointer's stable top-level value ID (falling back to
  /// node ID only when a value ID is unavailable).
  std::unordered_map<uint32_t, std::unordered_set<const llvm::CallBase *>>
      funPtrToIndCallSites;
  std::unordered_map<const llvm::CallBase *,
                     std::unordered_set<const llvm::Function *>>
      callSiteToConnectedCallees;
  /// @brief Reverse index: callee function -> indirect call sites that may
  /// invoke it. Mirrors SVF's CallGraph::getIndCallSitesInvokingCallee().
  std::unordered_map<const llvm::Function *,
                     std::unordered_set<const llvm::CallBase *>>
      calleeToIndCallSites;
  struct CallSiteCalleeKey {
    const llvm::CallBase *callSite = nullptr;
    const llvm::Function *callee = nullptr;
    bool operator==(const CallSiteCalleeKey &o) const {
      return callSite == o.callSite && callee == o.callee;
    }
  };
  struct CallSiteCalleeKeyHash {
    size_t operator()(const CallSiteCalleeKey &k) const noexcept {
      return std::hash<const void *>()(k.callSite) ^
             (std::hash<const void *>()(k.callee) << 1);
    }
  };
  std::unordered_map<CallSiteCalleeKey, uint32_t, CallSiteCalleeKeyHash>
      callSiteCalleeToId;
  uint32_t nextCallSiteId = 1;
  std::unique_ptr<LTCallGraph> refinedCallGraph;

public:
  /// @brief Constructor
  SVFG() : icfg(nullptr), nextNodeId(0) {}

  /// @brief Destructor
  ///
  virtual ~SVFG() {
    // First pass: delete all edges (each edge is in exactly one out-edge list).
    for (auto &pair : nodeMap) {
      SVFGNode *n = pair.second;
      if (!n)
        continue;
      // Copy the list because removeOutEdge modifies it.
      auto outEdges = n->getOutEdges();
      for (SVFGEdge *e : outEdges) {
        if (!e)
          continue;
        SVFGNode *dst = e->getDstNode();
        if (dst)
          dst->removeInEdge(e);
        n->removeOutEdge(e);
        delete e;
      }
    }
    // Second pass: delete nodes.
    for (auto &pair : nodeMap) {
      delete pair.second;
    }
  }

  //===------------------------------------------------------------------===
  // Node access
  //===------------------------------------------------------------------===

  inline SVFGNode *getNode(uint32_t id) const {
    auto it = nodeMap.find(id);
    return (it != nodeMap.end()) ? it->second : nullptr;
  }

  inline bool hasNode(uint32_t id) const {
    return nodeMap.find(id) != nodeMap.end();
  }

  inline uint32_t getNumNodes() const { return nodeMap.size(); }
  inline uint32_t getNextNodeId() { return nextNodeId++; }

  iterator begin() { return nodeMap.begin(); }
  iterator end() { return nodeMap.end(); }
  const_iterator begin() const { return nodeMap.begin(); }
  const_iterator end() const { return nodeMap.end(); }

  //===------------------------------------------------------------------===
  // Definition site access
  //===------------------------------------------------------------------===

  inline SVFGNode *getDef(const llvm::Instruction *inst) const {
    auto it = instToDefMap.find(inst);
    return (it != instToDefMap.end()) ? getNode(it->second) : nullptr;
  }

  inline bool hasDef(const llvm::Instruction *inst) const {
    return instToDefMap.find(inst) != instToDefMap.end();
  }

  inline void setDef(const llvm::Instruction *inst, uint32_t nodeId) {
    instToDefMap[inst] = nodeId;
  }

  inline SVFGNode *getMSSADef(uint32_t memReg, uint32_t version = 0) const {
    auto it = mssaVerToNodeMap.find(MemSSAKey{memReg, version});
    return (it != mssaVerToNodeMap.end()) ? it->second : nullptr;
  }

  inline void setMSSADef(uint32_t memReg, SVFGNode *node,
                         uint32_t version = 0) {
    mssaVerToNodeMap[MemSSAKey{memReg, version}] = node;
  }

  //===------------------------------------------------------------------===
  // Value mapping
  //===------------------------------------------------------------------===

  inline SVFGNode *getValueNode(const llvm::Value *val) const {
    auto it = valueToNodeMap.find(val);
    return (it != valueToNodeMap.end()) ? getNode(it->second) : nullptr;
  }

  inline const ValueToNodeMap &getValueNodeMap() const {
    return valueToNodeMap;
  }

  inline SVFGNode *getValueIdNode(uint32_t valueId) const {
    auto it = valueIdToDefNodeMap.find(valueId);
    return (it != valueIdToDefNodeMap.end()) ? getNode(it->second) : nullptr;
  }

  /// @brief Resolve a DDA abstract id to its canonical SVFG definition node.
  ///
  /// DDA ids live in the abstract value/object-id namespaces, not the raw
  /// SVFG node-id namespace:
  /// - value ids map to top-level defining nodes via getValueIdNode()
  /// - object ids map to the canonical defining Addr/value node for the
  ///   underlying allocation-site value
  ///
  /// Returns nullptr if the id is unknown to the current graph.
  SVFGNode *getCanonicalDefNodeForDDAId(uint32_t ddaId) const;

  inline void setValueNode(const llvm::Value *val, uint32_t nodeId) {
    valueToNodeMap[val] = nodeId;
    if (SVFGNode *node = getNode(nodeId)) {
      if (node->hasValueId())
        valueIdToDefNodeMap[node->getValueId()] = nodeId;
    }
  }

  //===------------------------------------------------------------------===
  // Node/edge management
  //===------------------------------------------------------------------===

  void addNode(SVFGNode *node);
  SVFGEdge *addEdge(SVFGNode *src, SVFGNode *dst, SVFGEdgeK kind,
                    const llvm::CallBase *callSite = nullptr,
                    const SVFGNodeBS &pointsTo = SVFGNodeBS(),
                    std::string callSiteDebug = std::string());
  void removeEdge(SVFGEdge *edge);
  void removeNode(SVFGNode *node);

  //===------------------------------------------------------------------===
  // Call/return site access
  //===------------------------------------------------------------------===

  inline const SVFGNodeSet &getActualIns(const llvm::CallBase *cs) const {
    static SVFGNodeSet empty;
    auto it = callSiteToActualInMap.find(cs);
    return (it != callSiteToActualInMap.end()) ? it->second : empty;
  }

  inline const SVFGNodeSet &getActualOuts(const llvm::CallBase *cs) const {
    static SVFGNodeSet empty;
    auto it = callSiteToActualOutMap.find(cs);
    return (it != callSiteToActualOutMap.end()) ? it->second : empty;
  }

  inline void addActualIn(const llvm::CallBase *cs, SVFGNode *node) {
    callSiteToActualInMap[cs].insert(node);
  }

  inline void addActualOut(const llvm::CallBase *cs, SVFGNode *node) {
    callSiteToActualOutMap[cs].insert(node);
  }

  inline const SVFGNodeSet &getActualParms(const llvm::CallBase *cs) const {
    static SVFGNodeSet empty;
    auto it = callSiteToActualParmMap.find(cs);
    return (it != callSiteToActualParmMap.end()) ? it->second : empty;
  }

  inline const SVFGNodeSet &getActualRets(const llvm::CallBase *cs) const {
    static SVFGNodeSet empty;
    auto it = callSiteToActualRetMap.find(cs);
    return (it != callSiteToActualRetMap.end()) ? it->second : empty;
  }

  inline void addActualParm(const llvm::CallBase *cs, SVFGNode *node) {
    callSiteToActualParmMap[cs].insert(node);
  }

  inline void addActualRet(const llvm::CallBase *cs, SVFGNode *node) {
    callSiteToActualRetMap[cs].insert(node);
  }

  inline const SVFGNodeSet &getFormalIns(const llvm::Function *f) const {
    static SVFGNodeSet empty;
    auto it = funcToFormalInMap.find(f);
    return (it != funcToFormalInMap.end()) ? it->second : empty;
  }

  inline const SVFGNodeSet &getFormalOuts(const llvm::Function *f) const {
    static SVFGNodeSet empty;
    auto it = funcToFormalOutMap.find(f);
    return (it != funcToFormalOutMap.end()) ? it->second : empty;
  }

  inline void addFormalIn(const llvm::Function *f, SVFGNode *node) {
    funcToFormalInMap[f].insert(node);
  }

  inline void addFormalOut(const llvm::Function *f, SVFGNode *node) {
    funcToFormalOutMap[f].insert(node);
  }

  inline const SVFGNodeSet &getFormalParms(const llvm::Function *f) const {
    static SVFGNodeSet empty;
    auto it = funcToFormalParmMap.find(f);
    return (it != funcToFormalParmMap.end()) ? it->second : empty;
  }

  inline const SVFGNodeSet &getFormalRets(const llvm::Function *f) const {
    static SVFGNodeSet empty;
    auto it = funcToFormalRetMap.find(f);
    return (it != funcToFormalRetMap.end()) ? it->second : empty;
  }

  inline void addFormalParm(const llvm::Function *f, SVFGNode *node) {
    funcToFormalParmMap[f].insert(node);
  }

  inline void addFormalRet(const llvm::Function *f, SVFGNode *node) {
    funcToFormalRetMap[f].insert(node);
  }

  //===------------------------------------------------------------------===
  // ICFG integration
  //===------------------------------------------------------------------===

  inline const ICFG *getICFG() const { return icfg; }
  inline void setICFG(const ICFG *i) { icfg = i; }

  //===------------------------------------------------------------------===
  // Statistics
  //===------------------------------------------------------------------===

  inline const SVFGStat &getStat() const { return stat; }
  inline SVFGStat &getStat() { return stat; }
  void updateStat(SVFGNode *node);
  void updateStat(SVFGEdge *edge);

  //===------------------------------------------------------------------===
  // Object debug
  //===------------------------------------------------------------------===

  inline void setObjectDebug(uint32_t objId, std::string label) {
    if (objId == 0)
      return;
    objectDebug[objId] = std::move(label);
  }

  inline const std::unordered_map<uint32_t, std::string> &
  getObjectDebugMap() const {
    return objectDebug;
  }

  /// @brief Map object ID (from edge pointsTo) to allocation-site Value*.
  /// Populated by the builder when PTA objects have getValue().
  /// Used by DDA for indirect-edge filtering and result conversion.
  void setObjectValue(uint32_t objId, const llvm::Value *v);
  /// @brief Return allocation-site Value* for objId, or nullptr if unknown.
  const llvm::Value *getObjectValue(uint32_t objId) const;
  /// @brief Return object ID for allocation-site Value* v, or 0 if unknown.
  uint32_t getObjectId(const llvm::Value *v) const;
  inline void addObjectForValue(const llvm::Value *value, uint32_t objId) {
    if (value && objId != 0)
      valueToObjIds[value].insert(objId);
  }
  inline const SVFGNodeBS &getObjectIds(const llvm::Value *value) const {
    static const SVFGNodeBS empty;
    auto it = valueToObjIds.find(value);
    return it == valueToObjIds.end() ? empty : it->second;
  }

  //===------------------------------------------------------------------===
  // Indirect callsite index (DDA)
  //===------------------------------------------------------------------===

  inline void addIndCallSite(uint32_t funPtrNodeId, const llvm::CallBase *cs) {
    if (!cs)
      return;
    funPtrToIndCallSites[funPtrNodeId].insert(cs);
  }

  inline const std::unordered_set<const llvm::CallBase *> &
  getIndCallSites(uint32_t funPtrNodeId) const {
    static const std::unordered_set<const llvm::CallBase *> empty;
    auto it = funPtrToIndCallSites.find(funPtrNodeId);
    return (it != funPtrToIndCallSites.end()) ? it->second : empty;
  }

  inline const std::unordered_map<uint32_t,
                                  std::unordered_set<const llvm::CallBase *>> &
  getIndCallSiteMap() const {
    return funPtrToIndCallSites;
  }

  /// @brief Register that \p callee may be invoked from indirect callsite \p
  /// cs.
  inline void addCalleeToIndCallSite(const llvm::Function *callee,
                                     const llvm::CallBase *cs) {
    if (callee && cs)
      calleeToIndCallSites[callee].insert(cs);
  }

  /// @brief Return all indirect call sites that may invoke \p callee.
  ///
  /// This is a potential-callee index, not just the subset whose inter edges
  /// have already been materialized. DDA uses it to proactively refine other
  /// unresolved callsites when entering a callee, mirroring SVF's call-graph
  /// side index.
  inline const std::unordered_set<const llvm::CallBase *> &
  getIndCallSitesInvokingCallee(const llvm::Function *callee) const {
    static const std::unordered_set<const llvm::CallBase *> empty;
    if (!callee)
      return empty;
    auto it = calleeToIndCallSites.find(callee);
    return (it != calleeToIndCallSites.end()) ? it->second : empty;
  }

  /// @brief Return true if (cs, callee) was newly marked as connected.
  ///
  /// This API is used by on-the-fly indirect-call resolution:
  /// once a function-pointer query discovers a concrete callee, builder/DDA
  /// records connectivity here and can then insert call/ret edges.
  inline bool markConnectedCallee(const llvm::CallBase *cs,
                                  const llvm::Function *callee) {
    if (!cs || !callee)
      return false;
    auto &s = callSiteToConnectedCallees[cs];
    const bool inserted = s.insert(callee).second;
    CallSiteCalleeKey key{cs, callee};
    if (callSiteCalleeToId.find(key) == callSiteCalleeToId.end())
      callSiteCalleeToId.emplace(key, nextCallSiteId++);
    // Populate reverse index: callee -> indirect call sites.
    if (!cs->getCalledFunction())
      calleeToIndCallSites[callee].insert(cs);
    if (!refinedCallGraph) {
      if (const llvm::Function *caller = cs->getFunction()) {
        if (const llvm::Module *M = caller->getParent())
          initializeRefinedCallGraph(*const_cast<llvm::Module *>(M));
      }
    }
    if (refinedCallGraph) {
      if (const llvm::Function *caller = cs->getFunction())
        refinedCallGraph->addResolvedCallEdge(cs, caller, callee);
    }
    return inserted;
  }

  inline bool hasConnectedCallee(const llvm::CallBase *cs,
                                 const llvm::Function *callee) const {
    auto it = callSiteToConnectedCallees.find(cs);
    if (it == callSiteToConnectedCallees.end())
      return false;
    return it->second.count(callee) != 0;
  }

  inline const std::unordered_set<const llvm::Function *> &
  getConnectedCallees(const llvm::CallBase *cs) const {
    static const std::unordered_set<const llvm::Function *> empty;
    auto it = callSiteToConnectedCallees.find(cs);
    return (it != callSiteToConnectedCallees.end()) ? it->second : empty;
  }

  void initializeRefinedCallGraph(llvm::Module &M);

  inline const LTCallGraph *getRefinedCallGraph() const {
    return refinedCallGraph.get();
  }

  /// @brief Return callsite ID for (cs, callee); 0 if not connected.
  uint32_t getCallSiteId(const llvm::CallBase *cs,
                         const llvm::Function *callee) const;

  /// @brief Set object metadata for an abstract object ID.
  ///
  /// Intended for first-write initialization. For refinement/merge paths,
  /// use updateObjectInfo().
  inline void setObjectInfo(uint32_t objId, const ObjectInfo &info) {
    if (objId == 0)
      return;
    objIdToInfo[objId] = info;
  }

  /// @brief Update (merge) object metadata for an abstract object ID.
  inline void updateObjectInfo(uint32_t objId, const ObjectInfo &info) {
    if (objId == 0)
      return;
    auto &dst = objIdToInfo[objId];
    dst.isHeap = dst.isHeap || info.isHeap;
    dst.isConcreteHeap = dst.isConcreteHeap || info.isConcreteHeap;
    dst.isStack = dst.isStack || info.isStack;
    dst.isGlobal = dst.isGlobal || info.isGlobal;
    dst.isFunction = dst.isFunction || info.isFunction;
    dst.isConstant = dst.isConstant || info.isConstant;
    dst.isFieldInsensitive = dst.isFieldInsensitive || info.isFieldInsensitive;
    dst.isArray = dst.isArray || info.isArray;
    dst.isUnknown = dst.isUnknown || info.isUnknown;
    dst.isSingleton = dst.isSingleton || info.isSingleton;
    if (dst.baseObjId == 0)
      dst.baseObjId = info.baseObjId;
  }

  /// @brief Get object metadata (nullptr if unknown).
  inline const ObjectInfo *getObjectInfo(uint32_t objId) const {
    auto it = objIdToInfo.find(objId);
    return (it != objIdToInfo.end()) ? &it->second : nullptr;
  }

  inline bool isHeapObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isHeap;
    return false;
  }
  inline bool isStackObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isStack;
    return false;
  }
  inline bool isConcreteHeapObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isConcreteHeap;
    return false;
  }
  inline bool isGlobalObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isGlobal;
    return false;
  }
  inline bool isFunctionObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isFunction;
    return false;
  }
  inline bool isConstantObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isConstant;
    return false;
  }
  inline bool isFieldInsensitiveObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isFieldInsensitive;
    return false;
  }
  inline bool isArrayObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isArray;
    return false;
  }
  inline bool isUnknownObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isUnknown;
    return false;
  }
  inline bool isSingletonObject(uint32_t objId) const {
    if (const ObjectInfo *info = getObjectInfo(objId))
      return info->isSingleton;
    return false;
  }
  inline SVFGNodeBS getFieldObjects(uint32_t objId) const {
    SVFGNodeBS fields;
    uint32_t base = objId;
    if (const ObjectInfo *info = getObjectInfo(objId))
      if (info->baseObjId != 0)
        base = info->baseObjId;
    for (const auto &entry : objIdToInfo) {
      const uint32_t candidateBase =
          entry.second.baseObjId != 0 ? entry.second.baseObjId : entry.first;
      if (candidateBase == base)
        fields.insert(entry.first);
    }
    if (fields.empty())
      fields.insert(objId);
    return fields;
  }

  inline void setNodeFunctionDebug(uint32_t nodeId, std::string label) {
    nodeFunctionDebug[nodeId] = std::move(label);
  }

  inline void setNodeCallSiteDebug(uint32_t nodeId, std::string label) {
    nodeCallSiteDebug[nodeId] = std::move(label);
  }

  inline std::string getNodeFunctionDebug(uint32_t nodeId) const {
    auto it = nodeFunctionDebug.find(nodeId);
    return (it != nodeFunctionDebug.end()) ? it->second : std::string();
  }

  inline std::string getNodeCallSiteDebug(uint32_t nodeId) const {
    auto it = nodeCallSiteDebug.find(nodeId);
    return (it != nodeCallSiteDebug.end()) ? it->second : std::string();
  }

  inline const std::unordered_map<uint32_t, std::string> &
  getNodeFunctionDebugMap() const {
    return nodeFunctionDebug;
  }

  inline const std::unordered_map<uint32_t, std::string> &
  getNodeCallSiteDebugMap() const {
    return nodeCallSiteDebug;
  }

  //===------------------------------------------------------------------===
  // Global store tracking (for global initializer handling)
  //===------------------------------------------------------------------===

  /// @brief Set of global store SVFG nodes (stores to globals in initializers)
  /// Mirrors SVF's globalVFGNodes for connectFromGlobalToProgEntry
  using GlobalSVFGNodeSet = std::set<SVFGNode *>;

  /// @brief Add a store node to the global store set
  inline void addGlobalStoreNode(SVFGNode *node) {
    if (node && node->isStmtNode()) {
      globalStoreNodes.insert(node);
    }
  }

  /// @brief Get all global store nodes
  inline const GlobalSVFGNodeSet &getGlobalStoreNodes() const {
    return globalStoreNodes;
  }

  /// @brief Get all global store nodes (mutable)
  inline GlobalSVFGNodeSet &getGlobalStoreNodes() { return globalStoreNodes; }

  /// @brief Check if a node is a global store node
  inline bool isGlobalStoreNode(SVFGNode *node) const {
    return globalStoreNodes.count(node) != 0;
  }

  //===------------------------------------------------------------------===
  // Graph algorithms
  //===------------------------------------------------------------------===

  /// @brief Get all predecessors (backward reachability)
  SVFGNodeSet getPreds(SVFGNode *node) const;

  /// @brief Get all successors (forward reachability)
  SVFGNodeSet getSuccs(SVFGNode *node) const;

  /// @brief Check if path exists between two nodes
  bool hasPath(SVFGNode *src, SVFGNode *dst) const;

  /// @brief Get intra-procedural VFG edge from src to dst with given kind.
  /// Used by DDA to find the direct edge from load/store pointer def to
  /// load/store. Returns nullptr if no such edge exists.
  SVFGEdge *getIntraVFGEdge(const SVFGNode *src, const SVFGNode *dst,
                            SVFGEdgeK kind) const;

  /// @brief Get the "LHS" node for direct value flow: the node that defines
  /// the value flowing out of \p node. For stmt/phi/param nodes this is the
  /// node itself. Used by DDA for backtraceAlongDirectVF.
  SVFGNode *getLHSTopLevPtr(SVFGNode *node) const;

  /// @brief Return callsite instruction if \p n is a callsite-ret SVFG node.
  const llvm::CallBase *isCallSiteRetSVFGNode(const SVFGNode *n) const;
  /// @brief Return function if \p n is a function-entry SVFG node.
  const llvm::Function *isFunEntrySVFGNode(const SVFGNode *n) const;

  /// @brief Collect existing inter-procedural value-flow edges for an
  /// indirect callsite-callee pair.
  ///
  /// This mirrors SVF's query intent for demand-driven refinement clients:
  /// return currently materialized call/return and memory inter edges that
  /// belong to (\p cs, \p callee).
  void getInterVFEdgesForIndirectCallSite(const llvm::CallBase *cs,
                                          const llvm::Function *callee,
                                          std::vector<SVFGEdge *> &edges) const;

  /// @brief Dump to DOT format
  ///
  /// The optional \p simple flag is kept for source compatibility with
  /// upstream SVF's dumping API. The current serializer-backed DOT writer
  /// does not implement alternate simple formatting and ignores the flag.
  void dump(const std::string &filename, bool simple = false) const;

  /// @brief Serialize graph to text
  bool writeToFile(const std::string &filename) const;

  /// @brief Deserialize graph from text
  bool readFromFile(const std::string &filename);

  /// @brief Print statistics
  void printStat() const;

  /// @brief Swap graph contents with another SVFG instance.
  void swapWith(SVFG &other);

  //===------------------------------------------------------------------===
  // Incremental update support
  //===------------------------------------------------------------------===

  /// @brief Mark nodes/edges that depend on points-to information for update
  /// Called when pointer analysis results change
  void markForUpdate(SVFGNode *node);

  /// @brief Get all nodes marked for update
  SVFGNodeSet getNodesForUpdate() const;

  /// @brief Clear update markers
  void clearUpdateMarkers();

private:
  /// @brief Nodes that need updating when PTA changes
  SVFGNodeSet nodesForUpdate;

  /// @brief Global store nodes (stores to globals in initializers)
  /// Used for connectFromGlobalToProgEntry to flow values from initializers to
  /// entry points
  GlobalSVFGNodeSet globalStoreNodes;
};

} // namespace analysis
} // namespace lotus
