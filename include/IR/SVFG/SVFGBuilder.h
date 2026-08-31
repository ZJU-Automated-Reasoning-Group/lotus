//===- SVFGBuilder.h -- SVFG Builder with AserPTA Integration
//---------------------//
//
//                     Lotus: Static Value-Flow Analysis
//
// Copyright (C) <2025>  <Lotus Development Team>
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
// SVFGBuilder: Production-ready builder using AserPTA for pointer analysis.
//
// This builder integrates with AserPTA (lib/Alias/InclusionBased/AserPTA) which provides:
// - Context-insensitive and k-call-site sensitive analysis
// - Field-sensitive and field-insensitive memory models
// - Multiple solver algorithms (Andersen, WavePropagation, DeepPropagation)
// - On-the-fly call graph construction
//
// Default configuration uses:
// - Context-insensitive analysis (NoCtx)
// - Field-sensitive memory model
// - Andersen solver (basic, fast, accurate enough for most cases)
//
// To use different configurations:
// - Context-sensitive: Use KCallSite<1>, KCallSite<2>, etc.
// - Different solver: WavePropagation, DeepPropagation, PartialUpdateSolver
// - Field-insensitive: Use FIMemModel instead of FSMemModel
//
// DDA-facing builder contract:
// - Produces guard-bearing memory value-flow edges where guards are sets of
//   abstract object IDs.
// - Maintains stable object metadata/value mappings (objId -> Value/info).
// - Provides unknown wildcard object ID for conservative fallback.
// - Optionally leaves indirect call edges unresolved so demand analyses can
//   connect them on-the-fly.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "IR/ICFG/CallGraph.h"
#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/MemoryRegionPartitioner.h"
#include "IR/SVFG/PointsToSetHash.h"
#include "IR/SVFG/SVFG.h"

#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace analysis {

/// @brief SVFGBuilder configuration
struct SVFGBuilderConfig {
  /// @brief Enable Memory SSA construction
  bool buildMSSA = true;

  /// @brief Use pointer analysis integration
  bool usePointerAnalysis = true;

  /// @brief Include global initializers
  bool includeGlobals = true;

  /// @brief Resolve indirect calls during SVFG construction (PTA-based).
  /// When false, SVFGBuilder will still create call/ret nodes but will not
  /// connect callsites to potential callees. DDA can then add such edges
  /// on-the-fly based on demand-driven points-to of function pointers.
  bool resolveIndirectCalls = true;

  /// @brief Maximum SSA version for memory regions
  uint32_t maxSSAVersion = std::numeric_limits<uint32_t>::max();

  /// @brief Solver type for AserPTA
  enum class SolverType {
    Andersen,        // Basic Andersen (default)
    WavePropagation, // SCC-based with differential propagation
    DeepPropagation, // Enhanced cycle detection
    PartialUpdate    // Hybrid with incremental updates
  } solverType = SolverType::Andersen;

  /// @brief Memory model type
  enum class MemModelType {
    FieldSensitive,  // FSMemModel (default)
    FieldInsensitive // FIMemModel
  } memModelType = MemModelType::FieldSensitive;

  /// Memory-region cover used by sparse MemorySSA.
  MemoryRegionPartitionStrategy memoryPartition =
      MemoryRegionPartitionStrategy::Distinct;

  /// @brief Constructor with defaults
  SVFGBuilderConfig() = default;
};

/// SVFGBuilder constructs SVFG from ICFG using AserPTA for pointer analysis.
///
/// The builder executes in these phases:
/// 1. Pointer analysis bootstrap (AserPTA) and object-ID mapping
/// 2. Node construction (statement, parameter, memory SSA nodes)
/// 3. Edge construction (value-flow, call/return, memory edges)
/// 4. Inter-procedural refinement (connect call/return edges)
/// 5. Memory SSA linking and optional optimization
///
/// Key design decisions:
/// - Object IDs are disjoint from SVFG node IDs (base = 1 << 30)
/// - Memory regions are versioned for Memory SSA
/// - Indirect calls can be resolved eagerly or deferred for DDA
/// - Points-to sets guard memory edges for precision
///
/// Example:
///   SVFGBuilder builder(config);
///   SVFG *svfg = builder.build(icfg);
class SVFGBuilder {
public:
  /// Value-symbol IDs live in a disjoint namespace from both SVFG node IDs and
  /// abstract object IDs.
  static constexpr uint32_t kValueIdBase = 1u << 29;

  /// Object IDs live in a disjoint namespace from SVFG node IDs.
  /// This avoids accidental collisions in DDA where `DPItem::cur` can hold
  /// either a pointer (SVFG node ID) or an abstract object ID.
  static constexpr uint32_t kObjIdBase = 1u << 30;

  /// @brief Memory region version info
  struct MemRegVer {
    uint32_t region;
    uint32_t version;
    bool operator==(const MemRegVer &other) const {
      return region == other.region && version == other.version;
    }
  };

  /// @brief Hash for MemRegVer
  struct MemRegVerHash {
    size_t operator()(const MemRegVer &mrv) const noexcept {
      return std::hash<uint32_t>()(mrv.region) ^
             (std::hash<uint32_t>()(mrv.version) << 1);
    }
  };

private:
  /// @brief Configuration
  SVFGBuilderConfig config;

  /// @brief Source ICFG
  const ICFG *icfg;

  /// @brief Built SVFG
  std::unique_ptr<SVFG> svfg;

  /// @brief Type-erased solver wrapper for safe storage and deletion
  struct SolverWrapper {
    enum class SolverKind { Wave, Deep, Basic };
    SolverKind kind;
    void *solver;

    SolverWrapper(SolverKind k, void *s) : kind(k), solver(s) {}
    
    void destroy();
    
    ~SolverWrapper() { destroy(); }
    
    // Non-copyable, movable
    SolverWrapper(const SolverWrapper &) = delete;
    SolverWrapper &operator=(const SolverWrapper &) = delete;
    SolverWrapper(SolverWrapper &&other) noexcept
        : kind(other.kind), solver(other.solver) {
      other.solver = nullptr;
    }
  };

  /// @brief AserPTA solver wrapper (type-safe deletion)
  std::unique_ptr<SolverWrapper> ptaSolverWrapper;
  
  /// @brief Previous points-to sets for change detection
  std::unordered_map<const llvm::Value *, std::vector<const void *>> previousPTSets;

  /// @brief Next node ID
  uint32_t nextNodeId;

  /// @brief Next memory region ID (separate from node IDs)
  uint32_t nextMemRegId;

  /// @brief Next top-level value-symbol ID.
  uint32_t nextValueId = kValueIdBase;

  /// @brief Value to SVFG node mapping
  std::unordered_map<const llvm::Value *, uint32_t> valueToNode;
  /// @brief LLVM value to canonical top-level value-symbol ID.
  std::unordered_map<const llvm::Value *, uint32_t> valueToValueId;
  /// @brief Synthetic value-symbol IDs for formal returns and varargs.
  std::unordered_map<const llvm::Function *, uint32_t> formalRetValueIds;
  std::unordered_map<const llvm::Function *, uint32_t> varArgValueIds;

  /// @brief PTA object to SVFG node mapping (for points-to set conversion)
  std::unordered_map<const void *, uint32_t> ptaObjectToObjId;
  /// @brief Reverse mapping for PTA object lookup by objId.
  std::unordered_map<uint32_t, const void *> objIdToPTAObject;
  /// @brief Field-insensitive object ID per base object.
  std::unordered_map<uint32_t, uint32_t> baseObjToFIObjId;

  /// @brief Singleton object ID used when a PTA object cannot be mapped.
  ///
  /// Semantics: wildcard object that may alias any object. This preserves
  /// soundness without creating unbounded numbers of dummy objects.
  uint32_t unknownObjId = 0;

  /// @brief Next object ID for points-to sets.
  uint32_t nextObjId = kObjIdBase;

  /// @brief Object ID to memory region mapping (one memReg per abstract object).
  std::unordered_map<uint32_t, uint32_t> objIdToMemReg;
  std::unordered_map<uint32_t, uint32_t> memRegToObjId;
  std::unordered_map<uint32_t, SVFGNodeBS> memRegToPts;

  /// @brief Canonical memory-region IDs keyed by points-to set.
  ///
  /// Upstream SVF's MemSSA uses "memory regions" (MRs) whose identity is
  /// derived from (field-sensitive) points-to regions, not from pointer SSA
  /// values. To approximate that invariant without SVFIR, we memoize a stable
  /// memReg ID per points-to set key.
  ///
  /// Uses hash-based lookup (O(1)) instead of string construction (O(n)).
  std::unordered_map<SVFGNodeBS, uint32_t, PointsToSetHash, PointsToSetEqual> ptsToMemReg;

  MemoryRegionPartitioner memoryRegionPartitioner;

  /// @brief Alloca instruction to memory region mapping
  std::unordered_map<const llvm::AllocaInst *, uint32_t> allocaToMemReg;

  /// @brief Global variable to memory region mapping
  std::unordered_map<const llvm::GlobalVariable *, uint32_t> globalToMemReg;

  /// @brief Load instruction to top-level Load SVFG node mapping
  std::unordered_map<const llvm::LoadInst *, uint32_t> loadToLoadNode;

  /// @brief Store instruction to top-level Store SVFG node mapping
  std::unordered_map<const llvm::StoreInst *, uint32_t> storeToStoreNode;

  /// @brief Memory region version mapping
  std::unordered_map<MemRegVer, uint32_t, MemRegVerHash> memRegVerToNode;

  /// @brief Function entry chi nodes
  std::unordered_map<const llvm::Function *, std::vector<uint32_t>>
      funcEntryChi;

  /// @brief Deduplication set for EntryChiSVFGNode creation (Bug #9 fix).
  /// Tracks which (entryFunc, memReg) pairs already have an EntryChi node so
  /// that re-visiting the same alloca/global via multiple uses does not create
  /// duplicate nodes.
  std::unordered_map<const llvm::Function *,
                     std::unordered_set<uint32_t>> funcEntryChiMemRegs;

  /// @brief Module-global memory regions seeded from the synthetic global-init node.
  std::unordered_map<uint32_t, SVFGNodeBS> globalEntryRegions;

  /// @brief Function exit mu nodes
  std::unordered_map<const llvm::Function *, std::vector<uint32_t>> funcExitMu;

  /// @brief Callsite actual-in nodes
  std::unordered_map<const llvm::CallBase *, std::vector<uint32_t>> csActualIn;

  /// @brief Callsite actual-out nodes
  std::unordered_map<const llvm::CallBase *, std::vector<uint32_t>> csActualOut;

  /// @brief Heap allocation to memory region mapping
  std::unordered_map<const llvm::Instruction *, uint32_t> heapAllocToMemReg;

  /// @brief Generic pointer value to memory region mapping fallback
  std::unordered_map<const llvm::Value *, uint32_t> ptrValToMemReg;

  /// @brief Call instruction to CallMu/CallChi nodes (one per accessed memReg).
  std::unordered_map<const llvm::CallBase *, std::vector<uint32_t>> callToMuNodes;
  std::unordered_map<const llvm::CallBase *, std::vector<uint32_t>> callToChiNodes;

  /// @brief Memory region version counter (for SSA versioning)
  /// Key: (Function*, memory region) -> version number
  /// This ensures versions are unique per function, avoiding collisions
  std::unordered_map<const llvm::Function *, std::unordered_map<uint32_t, uint32_t>> memRegVersion;

  /// @brief Basic block to memory PHI nodes map
  std::unordered_map<const llvm::BasicBlock *, std::unordered_map<uint32_t, uint32_t>> bbToMemPhi;

  /// @brief Function argument to memory regions (derived from points-to objects).
  std::unordered_map<const llvm::Argument *, std::vector<uint32_t>> argToMemRegs;

  /// @brief Value-flow edges at indirect call sites (for spurious-edge filtering).
  ///
  /// Used by optimization/update passes to avoid retaining stale speculative
  /// inter-procedural edges when points-to information changes.
  std::unordered_set<SVFGEdge *> vfEdgesAtIndCallSite;

  /// @brief Last graph returned by build(); used for compatibility accessors.
  SVFG *lastBuiltSVFG = nullptr;

  SVFG *getActiveSVFG() const { return svfg ? svfg.get() : lastBuiltSVFG; }

public:
  /// @brief Constructor
  SVFGBuilder(const SVFGBuilderConfig &cfg = SVFGBuilderConfig())
      : config(cfg), icfg(nullptr), svfg(nullptr), ptaSolverWrapper(nullptr),
        nextNodeId(0), nextMemRegId(1),
        memoryRegionPartitioner(cfg.memoryPartition) {}

  /// @brief Destructor
  ~SVFGBuilder();

  // Prevent copying
  SVFGBuilder(const SVFGBuilder &) = delete;
  SVFGBuilder &operator=(const SVFGBuilder &) = delete;

  //===------------------------------------------------------------------===
  // Build API
  //===------------------------------------------------------------------===

  /// @brief Build SVFG from ICFG with AserPTA
  /// @param icfg Input ICFG
  /// @return Built SVFG (owned by the builder until release via caller usage).
  SVFG *build(const ICFG *icfg);

  /// @brief Get object IDs for a pointer/alloc value using PTA (best-effort).
  ///
  /// AserPTA supports field-sensitive analysis via FSMemModel. When
  /// field-sensitivity is enabled (default), this returns:
  /// - For stack/heap allocations: one object ID per field accessed via GEP
  /// - For function pointers: the function object
  /// - For globals: field-sensitive objects if struct type
  ///
  /// Field-insensitive fallback (FIMemModel or large objects) returns a single
  /// base object ID for all fields.
  SVFGNodeBS getObjectIdsForValue(const llvm::Value *ptr);

  /// @brief Map base object + GEP to field object ID (field-sensitive if possible).
  ///
  /// Returns the field-sensitive object for GEP-derived pointer when:
  /// 1. FSMemModel is active (config.memModelType == FieldSensitive)
  /// 2. Base object is not marked field-insensitive
  /// 3. GEP has constant indices (PTA can compute field offset)
  ///
  /// Otherwise returns base object ID or 0 if lookup fails.
  uint32_t getGepObjectId(uint32_t baseObjId, const llvm::GetElementPtrInst *gep);

  /// @brief Get or create a field-insensitive object ID for base object.
  uint32_t getOrCreateFIObjId(uint32_t baseObjId);
  /// @brief Return the wildcard unknown object id, creating it if necessary.
  ///
  /// The unknown object is used as conservative fallback in both SVFG building
  /// (guard generation) and DDA out-of-budget fallback.
  uint32_t getUnknownObjId();

  /// @brief Get indirect call targets using pointer analysis
  std::vector<const llvm::Function *>
  getIndirectCallTargets(const llvm::CallBase *call);

  /// @brief Build SVFG with configuration
  ///
  /// Convenience overload that applies \p cfg then performs build().
  SVFG *build(const ICFG *icfg, const SVFGBuilderConfig &cfg);

  /// @brief Mark newly materialized indirect-call edges as feasible.
  ///
  /// Mirrors upstream SVF's builder bookkeeping for speculative edges added at
  /// indirect call sites. Demand-driven clients can remove edges they proved
  /// feasible from the spurious-edge set after refinement.
  inline void markValidVFEdges(const std::vector<SVFGEdge *> &edges) {
    for (SVFGEdge *edge : edges) {
      if (edge)
        vfEdgesAtIndCallSite.erase(edge);
    }
  }

  /// @brief Return true if \p edge was pre-connected speculatively at an
  /// indirect call site and has not yet been marked feasible.
  inline bool isSpuriousVFEdgeAtIndCallSite(const SVFGEdge *edge) const {
    return edge &&
           vfEdgesAtIndCallSite.find(const_cast<SVFGEdge *>(edge)) !=
               vfEdgesAtIndCallSite.end();
  }

  //===------------------------------------------------------------------===
  // Incremental update API
  //===------------------------------------------------------------------===

  /// @brief Update SVFG when pointer analysis results change
  /// This refreshes edges that depend on points-to sets.
  /// Note: For complex updates (StoreChi, PHI nodes, inter-procedural edges),
  /// a full rebuild may be more reliable than incremental update.
  /// @param svfg Existing SVFG to update
  /// @return true if update was successful
  bool updateSVFG(SVFG *svfg);

  /// @brief Update memory SSA edges for nodes marked for update
  ///
  /// This is best-effort incremental maintenance; full rebuild remains the
  /// correctness fallback for complex structural updates.
  void updateMemorySSAEdges(SVFG *svfg);

  /// @brief Get the explicit call-graph snapshot maintained by the builder.
  ///
  /// Direct-call edges come from the initial module scan; resolved indirect
  /// callees are appended as SVFG refinement materializes them.
  const LTCallGraph *getRefinedCallGraph() const {
    return lastBuiltSVFG ? lastBuiltSVFG->getRefinedCallGraph() : nullptr;
  }

  const MemoryRegionPartitioner::Statistics &
  getMemoryRegionPartitionStatistics() const {
    return memoryRegionPartitioner.statistics();
  }

  /// @brief SVF-style on-the-fly connection of an indirect callsite to a callee.
  ///
  /// When SVFGBuilderConfig::resolveIndirectCalls is false, SVFGBuilder builds
  /// Actual*/Formal* nodes but does not connect indirect callsites to callees.
  /// Demand-driven analyses (DDA) can call this method once a function-pointer
  /// target is discovered.
  ///
  /// @return true if any new edge was created.
  bool connectCallSiteToCalleeOnTheFly(SVFG *svfg, const llvm::CallBase *cs,
                                       const llvm::Function *callee,
                                       std::vector<SVFGEdge *> &newEdges);

  /// Materialize every indirect-call target admitted by the auxiliary
  /// inclusion-based pre-analysis. Exhaustive fspta uses this before solving;
  /// later flow-sensitive discoveries remain bounded by this target universe.
  std::size_t connectPreAnalysisIndirectCalls(SVFG *graph);

  //===------------------------------------------------------------------===
  // Builder phases
  //===------------------------------------------------------------------===

private:
  void initialize(const ICFG *cfg);
  void runPointerAnalysis();
  void buildNodes();
  void buildEdges();
  void initializeIndirectCallReverseIndex();
  void prepareMemoryRegionPartitioning();
  void buildMemorySSA();
  void buildMemoryPHINodes();
  void buildInterproceduralMemoryPHINodes();
  void connectMemorySSAEdges();
  void buildInterproceduralEdges();
  
  /// @brief Connect global initializer stores to program entry points
  /// Mirrors SVF's SVFG::connectFromGlobalToProgEntry
  /// Connects stores to globals in initializer functions to EntryChi nodes
  /// at the program entry (main or other entry functions)
  void connectFromGlobalToProgEntry();

  //===------------------------------------------------------------------===
  // Node building
  //===------------------------------------------------------------------===

  void buildTopLevelNodes();
  void buildAddressTakenNodes();
  void buildFormalParmNodes();
  void buildActualParmNodes();
  void buildFormalRetNodes();
  void buildActualRetNodes();
  void refreshStmtPointerNodeIds();

  //===------------------------------------------------------------------===
  // Edge building
  //===------------------------------------------------------------------===

  void buildCopyEdges();
  void buildDirectEdges();
  void buildLoadEdges();
  void buildStoreEdges();
  void buildGepEdges();
  void buildPhiEdges();
  void buildCmpEdges();
  void buildBranchEdges();
  void buildCallEdges();
  void buildReturnEdges();
  void buildMemoryEdges();

  //===------------------------------------------------------------------===
  // Helper methods
  //===------------------------------------------------------------------===

  /// @brief Get or create node ID for a value
  uint32_t getOrCreateNode(const llvm::Value *val);
  uint32_t getOrCreateValueId(const llvm::Value *val);
  uint32_t getOrCreateFormalRetValueId(const llvm::Function *F);
  uint32_t getOrCreateVarArgValueId(const llvm::Function *F);

  /// @brief Get or create memory region for an alloca
  uint32_t getOrCreateMemReg(const llvm::AllocaInst *alloca);

  /// @brief Get or create memory region for a global
  uint32_t getOrCreateMemReg(const llvm::GlobalVariable *global);

  /// @brief Get points-to set for a pointer value using AserPTA.
  /// @return A vector of PTA object handles (opaque to SVFG layer).
  std::vector<const void *> getPointsToSet(const llvm::Value *ptr);

  /// @brief Convert PTA objects to object IDs for points-to sets.
  ///
  /// When keepFunctions is false, function objects are filtered out.
  SVFGNodeBS convertPTAObjectsToObjIDs(const std::vector<const void *> &ptaObjects,
                                      bool keepFunctions = false);

  /// @brief Get or create a wildcard "unknown" object ID.
  ///
  /// Internal creator. Public read API is getUnknownObjId().
  uint32_t getOrCreateUnknownObjId();

  /// @brief Record an explicit caller->callee edge in the refined call graph.
  void recordRefinedCallEdge(const llvm::CallBase *call,
                             const llvm::Function *callee);


  /// @brief Get or create a canonical memory-region ID for a points-to set.
  ///
  /// When pts is empty, callers should fall back to value-based region IDs
  /// (e.g., getOrCreateMemReg(ptrVal)) to avoid collapsing unrelated unknowns.
  uint32_t getOrCreateMemRegForPointsTo(
      const SVFGNodeBS &pts, const llvm::Function *scope = nullptr);

  static const llvm::Function *getMemoryRegionScope(const llvm::Value *value);

  /// @brief Get or create a stable memory region for an abstract object ID.
  uint32_t getOrCreateMemRegForObject(uint32_t objId);

  /// @brief Get or create memory region for a heap allocation
  uint32_t getOrCreateMemReg(const llvm::Instruction *heapAlloc);

  /// @brief Get or create memory region for any pointer value
  uint32_t getOrCreateMemReg(const llvm::Value *ptrVal);


  /// @brief Check if instruction is a heap allocation (malloc/calloc/realloc)
  bool isHeapAllocation(const llvm::Instruction *inst) const;

  /// @brief Check whether pointer value should participate in Memory SSA.
  ///
  /// This is the gate for creating Mu/Chi/Phi memory nodes and guarded edges.
  bool isAddressTakenPointer(const llvm::Value *ptr) const;

  /// @brief Return true when two memory nodes may alias via points-to overlap.
  bool mayAliasMemoryNodes(const MSSASVFGNode *lhs, const MSSASVFGNode *rhs) const;

  /// @brief Create memory SSA version node
  uint32_t createMemRegVerNode(uint32_t memReg, uint32_t version,
                               const ICFGNode *icfgNode);

  /// @brief Create PHI node for a memory region at a merge point
  uint32_t createMemoryPHI(uint32_t memReg, const llvm::BasicBlock *bb);

  /// @brief Check if function might modify memory (basic function summary)
  bool mayReadMemory(const llvm::Function *F);
  bool mayReadMemory(const llvm::Function *F,
                     std::unordered_set<const llvm::Function *> &visited);
  bool mayModifyMemory(const llvm::Function *F);
  bool mayModifyMemory(const llvm::Function *F,
                       std::unordered_set<const llvm::Function *> &visited);
  bool callMayReadMemory(const llvm::CallBase *call);
  bool callMayModifyMemory(const llvm::CallBase *call);
  bool callArgMayReadMemory(const llvm::CallBase *call, unsigned argNo) const;
  bool callArgMayModifyMemory(const llvm::CallBase *call,
                              unsigned argNo) const;
  std::vector<const llvm::Function *> getRootFunctionsFromICFG() const;
  uint32_t getOrCreateCanonicalObjectIdForValue(const llvm::Value *v,
                                                SVFG::ObjectInfo info);
  uint32_t getCanonicalBaseObjId(uint32_t objId) const;
  uint32_t nextVersion(const llvm::Function *F, uint32_t memReg);

  /// @brief Get next node ID
  uint32_t nextNode() { return nextNodeId++; }
};

} // namespace analysis
} // namespace lotus

namespace llvm {
class CallBase;
class Function;
class GetElementPtrInst;
class Value;
} // namespace llvm
