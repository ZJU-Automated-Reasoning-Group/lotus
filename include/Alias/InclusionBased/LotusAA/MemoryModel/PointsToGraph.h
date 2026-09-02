/*
 * LotusAA - Points-To Graph Base Class
 * 
 * Base class for pointer analysis results. Provides common infrastructure
 * for managing points-to information, memory objects, and constraints.
 * 
 * Key Concepts:
 * - PTResult: Maps pointers to sets of (MemObject, offset) pairs
 * - MemObject: Abstract representation of memory locations
 * - Field-sensitive: Tracks individual struct fields separately
 */

/// @file PointsToGraph.h
/// @brief Points-to graph base class — per-function graph of (pointer → target
///        set) relationships
///
/// `PTGraph` is the per-function container for points-to information.  It
/// holds:
///   - `pt_results` — map from LLVM `Value *` to `PTResult *` (points-to sets)
///   - `mem_objs` — the set of `MemObject`s allocated in the function
///   - `global_objects` — objects corresponding to global variables
///   - `NullPTS` — a singleton null-pointer points-to set
///   - load-load matching caches (`load_category`, `load_category_collection`)
///   - object-to-call access-path depth caches
///   - path-condition caches (unit regions, AND/OR/NOT regions, phi regions,
///     block regions, semantic regions, call-target conditions)
///   - dominator tree for SSA-style locator versioning
///   - controlled-dependence info for path-condition construction
///
/// Subclasses (`IntraLotusAA`) implement the per-function analysis driver and
/// transfer functions.
///
/// ## PTResult
///
/// A points-to set for a single pointer.  Targets are either:
///   - **Direct**: `(ObjectLocator *)` — the pointer points to that locator
///   - **Derived**: `(PTResult *src, int64_t offset)` — inherits src's targets
///     plus an offset
///
/// Both carry a `path_cond_t` guard and are flattened on iteration via
/// `PTResultIterator`.

#pragma once

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

#include "Alias/InclusionBased/LotusAA/MemoryModel/MemObject.h"
#include "Alias/InclusionBased/LotusAA/MemoryModel/Types.h"
#include "Alias/InclusionBased/LotusAA/Support/Compat.h"

namespace llvm {

// Forward declarations
class LotusAA;
class PTResultIterator;
class PTGraph;

/*
 * PTResult - Points-to set for a pointer (simplified)
 * 
 * Two types of targets:
 * 1. Direct: <ObjectLocator> - ptr points to locator
 * 2. Derived: <PTResult', offset> - ptr points to (PTResult' + offset)
 */
/// Points-to set for a single pointer value.
///
/// Two target forms:
///   1. **Direct**:   `<path_cond_t, ObjectLocator>` — ptr → loc under cond
///   2. **Derived**:  `<path_cond_t, PTResult*, int64_t offset>` — ptr inherits
///      the targets of `src_pts` at additional `offset` under cond
///
/// `PTResultIterator` flattens both forms into a `map<ObjectLocator*, cond>`.
class PTResult {
public:
  // Direct points-to target
  struct PtItem {
    path_cond_t cond;
    ObjectLocator *locator;
    PtItem(path_cond_t cond, ObjectLocator *loc) : cond(cond), locator(loc) {}
    PtItem(const PtItem &item) : cond(item.cond), locator(item.locator) {}
  };

  // Derived points-to target  
  struct DerivedPtItem {
    path_cond_t cond;
    PTResult *src_pts;
    int64_t offset;

    DerivedPtItem(path_cond_t cond, PTResult *other_pts, int64_t off)
        : cond(cond), src_pts(other_pts), offset(off) {}
    DerivedPtItem(const DerivedPtItem &itemset)
        : cond(itemset.cond), src_pts(itemset.src_pts),
          offset(itemset.offset) {}
  };

private:
  std::vector<PtItem> pt_list;
  std::vector<DerivedPtItem> derived_list;
  Value *ptr;
  bool is_optimized;
  unsigned revision;

  friend class PTResultIterator;

public:
  PTResult(Value *ptr) : ptr(ptr), is_optimized(false), revision(0) {}

  Value *get_ptr() { return ptr; }
  unsigned getRevision() const { return revision; }

  void add_target(path_cond_t cond, MemObject *obj, int64_t offset) {
    ObjectLocator *locator = obj->findLocator(offset, true);
    pt_list.push_back({cond, locator});
    is_optimized = false;
    ++revision;
  }

  void add_derived_target(path_cond_t cond, PTResult *src_pts, int64_t offset) {
    derived_list.push_back({cond, src_pts, offset});
    is_optimized = false;
    ++revision;
  }
};

/*
 * PTResultIterator - Collect final points-to results
 */
/// Iterator that flattens a `PTResult` (including derived targets) into a
/// map from `ObjectLocator*` to `path_cond_t`.  Recursively visits the
/// derived-target graph with cycle detection (a `visited` set).  On first
/// construction caches the flattened result in the PTResult for O(1) reuse.
class PTResultIterator {
public:
  using iterator = std::map<ObjectLocator *, path_cond_t, obj_loc_cmp>::iterator;
  using size_type = std::map<ObjectLocator *, path_cond_t, obj_loc_cmp>::size_type;

private:
  std::map<ObjectLocator *, path_cond_t, obj_loc_cmp> res;
  PTGraph *parent_graph;

  void visit(PTResult *target, int64_t off, path_cond_t cond,
             std::set<PTResult *> &visited);

public:
  PTResultIterator(PTResult *target, PTGraph *parent_graph);

  iterator begin() { return res.begin(); }
  iterator end() { return res.end(); }
  size_type count(ObjectLocator *loc) { return res.count(loc); }
  path_cond_t get(ObjectLocator *loc) {
    assert(res.count(loc) && "The result does not contain the queried locator");
    return res[loc];
  }
  int size() { return res.size(); }

  friend raw_ostream &operator<<(raw_ostream &out, PTResultIterator &pt_it);
};

/*
 * PTGraph - Points-to graph manager (simplified)
 */
/// Per-function points-to graph manager.
///
/// Owns all `PTResult`, `MemObject`, and path-condition data for one function.
/// Provides the core operations needed by instruction transfer functions:
///   - `addPointsTo(ptr, obj, offset, cond)` — create a direct points-to edge
///   - `derivePtsFrom(ptr, src_pts, offset, cond)` — create a derived edge
///   - `assignPts(ptr, pts)` — assign a shared set (e.g. NullPTS)
///   - `loadPtrAt(ptr, from_loc, result)` — load values from memory locations
///   - `trackPtrRightValue(val, result)` — track through PHI/Select/Load
///   - `refineResult(to_refine)` — merge duplicate values with OR conditions
///
/// Path-condition queries (`getEmptyCond`, `getValueCond`, `getBlockCond`,
/// `findOrCreateUnitRegion`, `findOrCreateAndRegion`, etc.) are used by
/// transfer functions to attach guards to points-to targets.
class PTGraph {
public:
  enum PTGType { PTGBegin, PTGraphTy, IntraLotusAATy, PTGEnd };

  virtual PTGType getKind() const { return PTGraphTy; }

  static bool classof(const PTGraph *G) {
    return G->getKind() >= PTGBegin && G->getKind() <= PTGEnd;
  }

protected:
  // Parent function being analyzed
  Function *analyzed_func;

  // Parent LotusAA pass
  LotusAA *lotus_aa;

  // Dominance information for SSA construction
  DominatorTree *dom_tree;
  PostDominatorTree *post_dom_tree;

  // Special NULL result
  PTResult *NullPTS;

  // Points-to results
  std::map<Value *, PTResult *, llvm_cmp> pt_results;

  // Memory objects
  std::map<MemObject *, int, mem_obj_cmp> mem_objs;

  int pt_index;
  int obj_index;

  // Load-load matching
  std::map<LoadInst *, int, llvm_cmp> load_category;
  std::vector<std::set<LoadInst *, llvm_cmp> *> load_category_collection;
  bool load_load_match_performed;

  // Global objects
  std::set<MemObject *, mem_obj_cmp> global_objects;

  // Object-to-call access-path depth caches
  std::map<Value *, std::map<MemObject *, int, mem_obj_cmp>, llvm_cmp>
      object_call_arg_ap_depth_cache;
  std::map<Value *, std::set<MemObject *, mem_obj_cmp>, llvm_cmp>
      object_call_ap_depth_frontier;

  std::map<BasicBlock *,
           std::map<BasicBlock *, path_cond_t, llvm_cmp>, llvm_cmp>
      phi_region_cache;
  std::map<BasicBlock *,
           std::map<BasicBlock *, path_cond_t, llvm_cmp>, llvm_cmp>
      bb_region_cache;
  std::map<BasicBlock *, path_cond_t, llvm_cmp> unit_region_cache_;
  std::map<BasicBlock *,
           std::map<BasicBlock *, path_cond_t, llvm_cmp>, llvm_cmp>
      control_dep_cache_;
  std::map<std::pair<BasicBlock *, BasicBlock *>, path_cond_t> edge_cond_cache_;

  path_cond_t true_cond_;
  path_cond_t false_cond_;
  std::vector<std::unique_ptr<PathCond>> cond_nodes_;
  std::map<PathCond::ConstraintSummary, path_cond_t> formula_cond_cache_;
  std::map<std::pair<Value *, bool>, path_cond_t> value_cond_cache_;
  std::map<BasicBlock *, path_cond_t, llvm_cmp> block_cond_cache_;
  std::map<std::pair<Value *, Function *>, path_cond_t> call_target_cond_cache_;
  std::map<path_cond_t, path_cond_t> imported_cond_cache_;
  std::map<path_cond_t, path_cond_t> not_cond_cache_;
  std::map<std::pair<path_cond_t, path_cond_t>, path_cond_t> and_cond_cache_;
  std::map<std::pair<path_cond_t, path_cond_t>, path_cond_t> or_cond_cache_;
  bool control_dep_ready_;

  struct GuardedPointsToSignature {
    unsigned revision = 0;
    size_t fingerprint = 0;
    bool must_alias_eligible = false;
    std::vector<std::pair<ObjectLocator *, path_cond_t>> entries;
  };
  std::map<std::pair<PTResult *, int64_t>, GuardedPointsToSignature>
      guarded_points_to_signature_cache_;

  // Constants
  static const int VALUE_SEQ_UNDEF;
  static const int VALUE_SEQ_INFINITE;
  static const int FUNC_OBJ_UNREACHABLE;

protected:
  Type *normalizeType(Type *type);
  MemObject *newObject(Value *alloc_site,
                       MemObject::ObjKind obj_type = MemObject::CONCRETE);

  PTResult *addPointsTo(Value *ptr, MemObject *obj, int64_t offset,
                        path_cond_t cond);
  PTResult *derivePtsFrom(Value *ptr, PTResult *other_pts, int64_t offset,
                          path_cond_t cond);
  PTResult *assignPts(Value *ptr, PTResult *pts);

  void refineResult(mem_value_t &to_refine);

  void loadPtrAt(Value *ptr, Instruction *from_loc, mem_value_t &res,
                 bool create_symbol = false, int64_t offset = 0,
                 int func_level = ObjectLocator::FUNC_LEVEL_UNDEFINED,
                 ObjectLocator *func_call_cache = nullptr,
                 bool is_include_func_summary = false,
                 bool is_maintain_load_map = true,
                 const std::set<Instruction *, llvm_cmp>
                     *surviving_store_positions = nullptr);
  void loadPtrAtImpl(Value *ptr, Instruction *from_loc, mem_value_t &result,
                     bool create_symbol, int64_t query_offset, int func_level,
                     ObjectLocator *func_call_cache,
                     bool is_include_func_summary, bool is_maintain_load_map,
                     std::set<std::tuple<Value *, Instruction *, int64_t>>
                         &visited,
                     const std::set<Instruction *, llvm_cmp>
                         *surviving_store_positions);

  void trackPtrRightValue(Value *ptr, mem_value_t &res);
  void trackPtrRightValueUnderCondition(Value *ptr, mem_value_t &res,
                                        path_cond_t base_cond,
                                        float base_confidence);

  void performLoadLoadMatch();
  bool cacheLoadCategory(LoadInst *load_inst);
  void buildControlDependenceInfo();
  path_cond_t getCFGEdgeCond(BasicBlock *src_bb, BasicBlock *succ_bb);
  path_cond_t localizePathCond(path_cond_t cond);
  path_cond_t getComplementaryBranchCond(path_cond_t cond);
  path_cond_t internCond(std::unique_ptr<PathCond> cond);
  path_cond_t importPathCond(path_cond_t cond, Value *callsite,
                             Function *callee);
  const GuardedPointsToSignature &
  getGuardedPointsToSignature(PTResult *points_to, int64_t offset);

  virtual int getSequenceNum(Value *val) = 0;
  virtual int getInlineApDepth() = 0;
  virtual PTGraph *getPtGraph(Function *F) = 0;

public:
  PTGraph(Function *F, LotusAA *lotus_aa);
  virtual ~PTGraph();

  int getPtIndex() { return pt_index; }

  void getLoadValues(Value *ptr, Instruction *from_loc, mem_value_t &res,
                     int64_t offset = 0);

  bool isSameValue(Value *ptr1, Instruction *loc1, Value *ptr2,
                   Instruction *loc2, int64_t offset1 = 0,
                   int64_t offset2 = 0);
  bool isSameValue(LoadInst *l1, LoadInst *l2);

  /// Return the path condition under which two pointer values may designate
  /// the same concrete location.  Conditions for every shared guarded
  /// points-to target are combined directly, as in Tuna's aliasCond.
  path_cond_t getAliasCondition(Value *ptr1, Value *ptr2,
                                int64_t offset1 = 0, int64_t offset2 = 0);

  /// Conservatively prove that two pointers have syntactically equivalent
  /// guarded points-to sets.  A compact fingerprint rejects the common
  /// non-alias case; exact comparison on a hash match keeps the proof sound
  /// even in the presence of hash collisions.
  bool areMustAliases(Value *ptr1, Value *ptr2, int64_t offset1 = 0,
                      int64_t offset2 = 0);

  const std::set<LoadInst *, llvm_cmp> &
  getAllLoadWithSameValue(LoadInst *load_inst);

  PTResult *findPTResult(Value *ptr, bool is_create = false);

  // Get access-path depth of object to call arguments
  int getObjectToCallApDepth(MemObject *obj, CallInst *call);

  // Utilities
  const DataLayout &getDL();
  Function *getFunc() { return analyzed_func; }
  DominatorTree *getDomTree() { return dom_tree; }
  PTResult *getNullPTS() { return NullPTS; }

  int getObjectID(MemObject *obj) {
    auto it = mem_objs.find(obj);
    assert(it != mem_objs.end() && "Object not in this PTG");
    return it->second;
  }

  void dumpMemObjs();

  path_cond_t getEmptyCond();
  path_cond_t getFalseCond();
  bool isAlwaysSatisfied(path_cond_t cond) const;
  bool isSatisfiable(path_cond_t cond) const;
  bool isNoEffectFunction(Function *F) const;
  path_cond_t getValueCond(Value *value, bool sense = true);
  path_cond_t getBlockCond(BasicBlock *BB);
  path_cond_t getUnitRegion(BasicBlock *BB);
  path_cond_t getCallTargetCond(Value *called_value, Function *callee);
  path_cond_t findOrCreateBBRegion(BasicBlock *src_bb, BasicBlock *target_bb);
  path_cond_t findOrCreateAndRegion(path_cond_t lhs, path_cond_t rhs);
  path_cond_t findOrCreateOrRegion(path_cond_t lhs, path_cond_t rhs);
  path_cond_t findOrCreateNotRegion(path_cond_t cond);
  path_cond_t findOrCreateUnitPhiRegion(BasicBlock *cur_bb,
                                        BasicBlock *incoming_bb);

  friend class MemObject;
  friend class ObjectLocator;
  friend class PTResultIterator;

  static Type *DEFAULT_POINTER_TYPE;
  static Type *DEFAULT_NON_POINTER_TYPE;
  static constexpr int64_t UNKNOWN_OFFSET = std::numeric_limits<int64_t>::max();

  static bool isUnknownOffset(int64_t offset) {
    return offset == UNKNOWN_OFFSET;
  }

  static int64_t composeOffset(int64_t base, int64_t delta) {
    if (isUnknownOffset(base) || isUnknownOffset(delta))
      return UNKNOWN_OFFSET;
    return base + delta;
  }
};

} // namespace llvm
