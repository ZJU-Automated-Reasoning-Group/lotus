/// @file PointsToGraph.cpp
/// @brief Points-to graph data structure and core operations
///
/// This file implements `PTGraph` and `PTResult`, the foundational data
/// structures for representing and querying **points-to relationships** in
/// LotusAA.
///
/// **Core Data Structures:**
///
/// 1. **PTResult**: Points-to set for a single pointer value
///    - Direct targets: `{(obj1, offset1), (obj2, offset2), ...}`
///    - Derived targets: `{ptr → offset}` (indirection through another pointer)
///    - Cached and memoized for performance
///
/// 2. **PTResultIterator**: Efficient traversal of points-to sets
///    - Recursively expands derived targets
///    - Handles cycles gracefully
///    - Caches results for repeated queries
///
/// 3. **PTGraph**: Per-function points-to graph
///    - Maps `Value* → PTResult*` (points-to sets)
///    - Owns all `MemObject`s for the function
///    - Provides utilities for memory operations and value tracking
///
/// **Key Operations:**
///
/// - `addPointsTo(ptr, obj, offset)`: Create direct points-to edge
/// - `derivePtsFrom(ptr, other_pts, offset)`: Create derived edge
/// - `loadPtrAt(ptr, inst, result)`: Load values from memory locations
/// - `trackPtrRightValue(val, result)`: Track value through PHI/Select/Load
///
/// **Memory Model Integration:**
/// ```
/// PTGraph
///   ├── pt_results: Value → PTResult
///   ├── mem_objs: MemObject set
///   └── Each MemObject contains ObjectLocators
///       └── Each ObjectLocator tracks stored values
/// ```
///
/// **Optimization Techniques:**
///
/// 1. **Memoization**: PTResults cached by Value*
/// 2. **Iterator Caching**: PTResultIterator results cached in PTResult
/// 3. **Load Categories**: Group equivalent loads for load-load matching
/// 4. **Access Path Depth Limiting**: Prune deep field accesses for scalability
///
/// **Configuration:**
/// - `lotus_restrict_pts_count`: Max points-to set size (default: 100)
/// - `lotus_restrict_obj_ap_depth`: Max access path depth for objects (default:
/// 5)
///
/// **Special Values:**
/// - `NullPTS`: Singleton for null pointer
/// - `DEFAULT_NON_POINTER_TYPE`: Int64 (placeholder for non-pointers)
/// - `DEFAULT_POINTER_TYPE`: Int8* (generic pointer type)
///
/// @see PTResult for points-to set representation
/// @see PTResultIterator for efficient set traversal
/// @see MemObject for memory object abstraction
/// @see ObjectLocator for field-level tracking

#include "Alias/InclusionBased/LotusAA/MemoryModel/PointsToGraph.h"

#include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"
#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/InclusionBased/LotusAA/Support/Config.h"

#include <llvm/Support/ErrorHandling.h>

#include <functional>
#include <set>
#include <tuple>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/DominanceFrontier.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CommandLine.h>

using namespace llvm;
using namespace std;

static cl::opt<int> lotus_restrict_pts_count(
    "lotus-restrict-pts-count",
    cl::desc("Maximum number of locators a pointer may point to"), cl::init(3),
    cl::Hidden);

static cl::opt<int> lotus_restrict_obj_ap_depth(
    "lotus-restrict-obj-ap-depth",
    cl::desc("Maximum AP-depth of objects considered for callees"), cl::init(5),
    cl::Hidden);

namespace {

template <typename T> static pair<T, T> canonicalCondPair(T lhs, T rhs) {
  return (lhs < rhs) ? make_pair(lhs, rhs) : make_pair(rhs, lhs);
}

static bool canSplitSelectCondition(Value *cond) {
  if (!cond || isa<UndefValue>(cond) || isa<PoisonValue>(cond))
    return false;

  if (isa<Instruction>(cond) || isa<Argument>(cond))
    return true;

  if (auto *CI = dyn_cast<ConstantInt>(cond))
    return CI->getBitWidth() == 1;

  return false;
}

static bool isCompositeCond(path_cond_t cond) {
  if (!cond)
    return false;
  switch (cond->getKind()) {
  case PathCond::Kind::And:
  case PathCond::Kind::Or:
  case PathCond::Kind::Not:
    return true;
  default:
    return false;
  }
}

static Type *getFieldTypeAtOffset(Type *type, int64_t offset,
                                  const DataLayout &DL) {
  if (!type || PTGraph::isUnknownOffset(offset) || offset <= 0)
    return type;

  if (auto *array_ty = dyn_cast<ArrayType>(type)) {
    uint64_t elem_size = DL.getTypeSizeInBits(array_ty->getElementType());
    if (elem_size == 0)
      return type;
    uint64_t idx = static_cast<uint64_t>(offset) / elem_size;
    uint64_t remainder = static_cast<uint64_t>(offset) % elem_size;
    if (idx >= array_ty->getNumElements())
      return type;
    return getFieldTypeAtOffset(array_ty->getElementType(),
                                static_cast<int64_t>(remainder), DL);
  }

  if (auto *struct_ty = dyn_cast<StructType>(type)) {
    int n_elem = struct_ty->getNumContainedTypes();
    int cur_size = 0;
    int last_size = 0;
    int idx;
    for (idx = 0; idx < n_elem; idx++) {
      if ((uint64_t)cur_size >= (uint64_t)offset)
        break;

      Type *t = struct_ty->getContainedType(idx);
      last_size = cur_size;
      cur_size += (int)DL.getTypeSizeInBits(t);
    }

    if ((uint64_t)cur_size == (uint64_t)offset && idx < n_elem)
      return getFieldTypeAtOffset(struct_ty->getContainedType(idx), 0, DL);
    if ((uint64_t)cur_size > (uint64_t)offset &&
        (uint64_t)last_size < (uint64_t)offset) {
      return getFieldTypeAtOffset(struct_ty->getContainedType(idx - 1),
                                  offset - last_size, DL);
    }
    return type;
  }

  if (auto *vec_ty = dyn_cast<VectorType>(type)) {
    Type *elem_type = vec_ty->getElementType();
    uint64_t elem_size = DL.getTypeSizeInBits(elem_type);
    if (elem_size == 0)
      return type;
    uint64_t idx = static_cast<uint64_t>(offset) / elem_size;
    uint64_t remainder = static_cast<uint64_t>(offset) % elem_size;
    if (idx >= vec_ty->getElementCount().getKnownMinValue())
      return type;
    return getFieldTypeAtOffset(elem_type, static_cast<int64_t>(remainder), DL);
  }

  return type;
}

} // namespace

// Static members
Type *PTGraph::DEFAULT_NON_POINTER_TYPE = nullptr;
Type *PTGraph::DEFAULT_POINTER_TYPE = nullptr;
const int PTGraph::VALUE_SEQ_UNDEF = -1;
const int PTGraph::VALUE_SEQ_INFINITE = -2;
const int PTGraph::FUNC_OBJ_UNREACHABLE = -1;

// PTResultIterator
PTResultIterator::PTResultIterator(PTResult *target, PTGraph *parent_graph)
    : parent_graph(parent_graph) {
  set<PTResult *> visited;
  visit(target, 0, parent_graph->getEmptyCond(), visited);

  // Optimize: cache results in target
  if (!target->is_optimized) {
    target->derived_list.clear();
    target->pt_list.clear();
    int count = 0;
    for (auto &item : res) {
      count++;
      if (lotus_restrict_pts_count != -1 && count > lotus_restrict_pts_count)
        break;
      target->pt_list.push_back(PTResult::PtItem(item.second, item.first));
    }
    target->is_optimized = true;
  }
}

void PTResultIterator::visit(PTResult *target, int64_t off, path_cond_t cond,
                             set<PTResult *> &visited) {
  // In fuzzing / partially-built summaries we can see null derived targets.
  // Don't crash in release builds; just treat them as empty.
  if (!target)
    return;

  // Check for cycles - if already visited, skip
  if (visited.count(target))
    return;

  visited.insert(target);

  // Direct targets
  for (PTResult::PtItem &item : target->pt_list) {
    if (!item.locator)
      continue;
    ObjectLocator *locator = item.locator->offsetBy(off);
    if (!locator)
      continue;
    path_cond_t new_cond = parent_graph->findOrCreateAndRegion(cond, item.cond);
    auto it = res.find(locator);
    if (it == res.end()) {
      res.insert(make_pair(locator, new_cond));
    } else {
      it->second = parent_graph->findOrCreateOrRegion(it->second, new_cond);
    }
  }

  // Derived targets
  for (PTResult::DerivedPtItem &item : target->derived_list) {
    if (!item.src_pts)
      continue;
    path_cond_t new_cond = parent_graph->findOrCreateAndRegion(cond, item.cond);
    visit(item.src_pts, off + item.offset, new_cond, visited);
  }

  visited.erase(target);
}

namespace llvm {

raw_ostream &operator<<(raw_ostream &out, PTResultIterator &pt_it) {
  for (auto it = pt_it.begin(); it != pt_it.end(); ++it) {
    out << "  " << *it->first << "\n";
  }
  return out;
}

} // namespace llvm

// PTGraph
PTGraph::PTGraph(Function *F, LotusAA *lotus_aa)
    : analyzed_func(F), lotus_aa(lotus_aa), pt_index(0), obj_index(0),
      load_load_match_performed(false), control_dep_ready_(false) {
  // Get dominance information
  dom_tree = lotus_aa->getDomTree(F);
  post_dom_tree = new PostDominatorTree(*F);

  true_cond_ = nullptr;
  false_cond_ = nullptr;

  // Create NULL points-to result
  NullPTS = addPointsTo(nullptr, MemObject::NullObj, 0, getEmptyCond());
}

PTGraph::~PTGraph() {
  delete NullPTS;

  for (auto &it : pt_results) {
    if (it.second != NullPTS)
      delete it.second;
  }

  for (auto &obj : mem_objs) {
    delete obj.first;
  }

  for (auto &category : load_category_collection) {
    delete category;
  }

  delete post_dom_tree;
}

PTResult *PTGraph::findPTResult(Value *ptr, bool is_create) {
  auto it = pt_results.find(ptr);
  if (it != pt_results.end())
    return it->second;

  if (is_create) {
    PTResult *pts = new PTResult(ptr);
    pt_results[ptr] = pts;
    return pts;
  }

  return nullptr;
}

MemObject *PTGraph::newObject(Value *alloc_site, MemObject::ObjKind obj_type) {
  MemObject *obj = (obj_type == MemObject::CONCRETE)
                       ? new MemObject(alloc_site, this, obj_type)
                       : new SymbolicMemObject(alloc_site, this);

  if (isa_and_nonnull<GlobalValue>(alloc_site)) {
    global_objects.insert(obj);
  }

  mem_objs[obj] = obj_index++;
  return obj;
}

PTResult *PTGraph::addPointsTo(Value *ptr, MemObject *obj, int64_t offset,
                               path_cond_t cond) {
  if (pt_results.find(ptr) != pt_results.end())
    llvm::report_fatal_error("Re-assigning value -- violating SSA");
  PTResult *pts = findPTResult(ptr, true);
  pts->add_target(cond, obj, offset);
  return pts;
}

PTResult *PTGraph::derivePtsFrom(Value *ptr, PTResult *other_pts,
                                 int64_t offset, path_cond_t cond) {
  if (pt_results.find(ptr) != pt_results.end())
    llvm::report_fatal_error("Re-assigning value -- violating SSA");
  PTResult *pts = findPTResult(ptr, true);
  pts->add_derived_target(cond, other_pts, offset);
  return pts;
}

PTResult *PTGraph::assignPts(Value *ptr, PTResult *pts) {
  pt_results[ptr] = pts;
  return pts;
}

Type *PTGraph::normalizeType(Type *type) {
  assert(type && "Normalizing NULL type");
  return type; // Use original type
}

void PTGraph::refineResult(mem_value_t &to_refine) {
  map<Value *, map<Instruction *, pair<path_cond_t, float>, llvm_cmp>, llvm_cmp>
      tmp_to_merge_values;
  for (auto &val_struct : to_refine) {
    path_cond_t cond = val_struct.cond;
    Value *val = val_struct.val;
    Instruction *pos = val_struct.pos;
    float confidence = val_struct.confidence;
    if (tmp_to_merge_values.count(val) == 0 ||
        tmp_to_merge_values[val].count(pos) == 0) {
      tmp_to_merge_values[val][pos] = make_pair(cond, confidence);
    } else {
      path_cond_t pre_cond = tmp_to_merge_values[val][pos].first;
      float pre_confidence = tmp_to_merge_values[val][pos].second;
      tmp_to_merge_values[val][pos].first =
          findOrCreateOrRegion(pre_cond, cond);
      tmp_to_merge_values[val][pos].second =
          mem_value_item_t::compute_or_confidence(pre_confidence, confidence);
    }
  }

  to_refine.clear();
  for (auto &val_pair : tmp_to_merge_values) {
    for (auto &pos_cond_pair : val_pair.second) {
      to_refine.push_back(mem_value_item_t(pos_cond_pair.second.first,
                                           pos_cond_pair.first, val_pair.first,
                                           pos_cond_pair.second.second));
    }
  }
}

void PTGraph::trackPtrRightValue(Value *ptr, mem_value_t &res) {
  trackPtrRightValueUnderCondition(ptr, res, getEmptyCond(), 1.0f);
  refineResult(res);
}

void PTGraph::trackPtrRightValueUnderCondition(Value *ptr, mem_value_t &res,
                                               path_cond_t base_cond,
                                               float base_confidence) {
  if (IntraLotusAAConfig::lotus_restrict_right_value_count != -1 &&
      static_cast<int>(res.size()) >=
          IntraLotusAAConfig::lotus_restrict_right_value_count)
    return;

  if (Argument *arg = dyn_cast<Argument>(ptr)) {
    res.push_back(mem_value_item_t(base_cond, nullptr, arg, base_confidence));
  } else if (LoadInst *load = dyn_cast<LoadInst>(ptr)) {
    mem_value_t load_result;
    getLoadValues(load->getPointerOperand(), load, load_result);
    for (auto &item : load_result) {
      path_cond_t final_cond = findOrCreateAndRegion(base_cond, item.cond);
      float final_confidence = mem_value_item_t::compute_and_confidence(
          base_confidence, item.confidence);
      trackPtrRightValueUnderCondition(item.val, res, final_cond,
                                       final_confidence);
    }
  } else if (PHINode *phi = dyn_cast<PHINode>(ptr)) {
    for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
      Value *incoming_val = phi->getIncomingValue(i);
      BasicBlock *incoming_bb = phi->getIncomingBlock(i);
      path_cond_t phi_cond =
          findOrCreateUnitPhiRegion(phi->getParent(), incoming_bb);
      trackPtrRightValueUnderCondition(
          incoming_val, res, findOrCreateAndRegion(base_cond, phi_cond),
          base_confidence);
    }
  } else if (SelectInst *sel = dyn_cast<SelectInst>(ptr)) {
    if (!canSplitSelectCondition(sel->getCondition())) {
      // Falcon keeps an unmodeled select opaque in right-value propagation
      // instead of inventing a fresh path split.
      res.push_back(mem_value_item_t(base_cond, sel, ptr, base_confidence));
    } else {
      path_cond_t true_cond = getValueCond(sel->getCondition(), true);
      path_cond_t false_cond = getValueCond(sel->getCondition(), false);
      trackPtrRightValueUnderCondition(sel->getTrueValue(), res,
                                       findOrCreateAndRegion(base_cond,
                                                             true_cond),
                                       base_confidence);
      trackPtrRightValueUnderCondition(sel->getFalseValue(), res,
                                       findOrCreateAndRegion(base_cond,
                                                             false_cond),
                                       base_confidence);
    }
  } else if (CastInst *cast = dyn_cast<CastInst>(ptr)) {
    trackPtrRightValueUnderCondition(cast->getOperand(0), res, base_cond,
                                     base_confidence);
  } else {
    res.push_back(mem_value_item_t(base_cond, dyn_cast<Instruction>(ptr), ptr,
                                   base_confidence));
  }
}

void PTGraph::getLoadValues(Value *ptr, Instruction *from_loc, mem_value_t &res,
                            int64_t offset) {
  loadPtrAt(ptr, from_loc, res, false, offset);
}

void PTGraph::loadPtrAt(Value *ptr, Instruction *from_loc, mem_value_t &result,
                        bool create_symbol, int64_t query_offset,
                        int func_level, ObjectLocator *func_call_cache,
                        bool is_include_func_summary,
                        bool is_maintain_load_map,
                        const std::set<Instruction *, llvm_cmp>
                            *surviving_store_positions) {
  // Use visited set to prevent infinite recursion
  std::set<std::tuple<Value *, Instruction *, int64_t>> visited;
  loadPtrAtImpl(ptr, from_loc, result, create_symbol, query_offset, func_level,
                func_call_cache, is_include_func_summary, is_maintain_load_map,
                visited, surviving_store_positions);
}

void PTGraph::loadPtrAtImpl(
    Value *ptr, Instruction *from_loc, mem_value_t &result, bool create_symbol,
    int64_t query_offset, int func_level, ObjectLocator *func_call_cache,
    bool is_include_func_summary, bool is_maintain_load_map,
    std::set<std::tuple<Value *, Instruction *, int64_t>> &visited,
    const std::set<Instruction *, llvm_cmp> *surviving_store_positions) {
  // Defensive: callers should pass real IR values/locations, but fuzzing and
  // summary edges can route nullptrs here. Avoid null-deref inside type queries
  // and ObjectLocator::getValues() (which requires a non-null Instruction*).
  if (!ptr)
    return;

  // Cycle detection - prevent infinite recursion
  std::tuple<Value *, Instruction *, int64_t> key(ptr, from_loc, query_offset);
  if (visited.count(key))
    return;
  visited.insert(key);

  PTResult *ptr_pts = findPTResult(ptr);
  if (!ptr_pts) {
    // Return early if no points-to result (instead of asserting)
    return;
  }

  Type *value_type = nullptr;
  if (create_symbol) {
    PointerType *ptr_type = dyn_cast<PointerType>(ptr->getType());
    if (ptr_type) {
      value_type = getPointerElementTypeCompat(ptr_type, &getDL());
      value_type = getFieldTypeAtOffset(value_type, query_offset, getDL());
    } else {
      value_type = DEFAULT_NON_POINTER_TYPE;
    }
  }

  // Collect all points-to targets
  PTResultIterator iter(ptr_pts, this);
  if (lotus_restrict_pts_count != -1 && iter.size() > lotus_restrict_pts_count)
    return;

  for (auto &point_to_item : iter) {
    ObjectLocator *loc = point_to_item.first;
    int64_t offset = loc->getOffset();
    MemObject *obj = loc->getObj();

    // Skip null and unknown objects
    if (obj->isNull() || obj->isUnknown())
      continue;

    // Adjust offset if query_offset provided
    if (query_offset != 0) {
      loc =
          obj->findLocator(PTGraph::composeOffset(offset, query_offset), true);
    }

    // Get values from this locator
    mem_value_t tmp_result;
    if (from_loc) {
      loc->getValues(from_loc, point_to_item.second, tmp_result, value_type,
                     func_level, true, func_call_cache,
                     is_include_func_summary, surviving_store_positions);
    } else {
      // No program point: fall back to the coarse per-object stored-value cache
      // at this offset. This is conservative and avoids requiring dominance
      // info.
      const int64_t off_key = PTGraph::composeOffset(offset, query_offset);
      auto &stored = obj->getStoredValues();
      auto it = stored.find(off_key);
      if (it != stored.end()) {
        for (Value *v : it->second) {
          tmp_result.push_back(
              mem_value_item_t(point_to_item.second, nullptr, v));
        }
      }

      // If nothing known was stored, mirror ObjectLocator::getValues() default.
      if (tmp_result.empty()) {
        tmp_result.push_back(mem_value_item_t(point_to_item.second, nullptr,
                                              obj->isReallyAllocated()
                                                  ? LocValue::UNDEF_VALUE
                                                  : LocValue::FREE_VARIABLE));
      }
    }

    result.insert(result.end(), tmp_result.begin(), tmp_result.end());

    // Track loaded values for load instructions
    if (is_maintain_load_map && from_loc &&
        (isa<LoadInst>(from_loc) || isa<CallBase>(from_loc))) {
      Value *val = from_loc;
      if (isa<LoadInst>(from_loc))
        val = from_loc;
      obj->getLoadedValues()[PTGraph::composeOffset(offset, query_offset)]
          .insert(val);
    }
  }
}

bool PTGraph::cacheLoadCategory(LoadInst *load_inst) {
  for (unsigned idx = 0; idx < load_category_collection.size(); idx++) {
    assert(!load_category_collection[idx]->empty());
    LoadInst *rep = *load_category_collection[idx]->begin();
    if (isSameValue(load_inst, rep)) {
      load_category_collection[idx]->insert(load_inst);
      load_category[load_inst] = idx;
      return false;
    }
  }

  // New category
  load_category[load_inst] = load_category_collection.size();
  auto *new_category = new set<LoadInst *, llvm_cmp>;
  new_category->insert(load_inst);
  load_category_collection.push_back(new_category);
  return true;
}

void PTGraph::performLoadLoadMatch() {
  if (load_load_match_performed)
    return;

  for (BasicBlock &B : *analyzed_func) {
    for (Instruction &I : B) {
      if (LoadInst *load = dyn_cast<LoadInst>(&I)) {
        cacheLoadCategory(load);
      }
    }
  }

  load_load_match_performed = true;
}

const set<LoadInst *, llvm_cmp> &
PTGraph::getAllLoadWithSameValue(LoadInst *load_inst) {
  if (!load_category.count(load_inst))
    performLoadLoadMatch();
  assert(load_category.count(load_inst));
  int idx = load_category[load_inst];
  return *load_category_collection[idx];
}

bool PTGraph::isSameValue(LoadInst *l1, LoadInst *l2) {
  if (!load_category.empty() && load_category.count(l1) &&
      load_category.count(l2)) {
    return load_category[l1] == load_category[l2];
  }
  return isSameValue(l1->getPointerOperand(), l1, l2->getPointerOperand(), l2);
}

bool PTGraph::isSameValue(Value *ptr1, Instruction *pos1, Value *ptr2,
                          Instruction *pos2, int64_t offset1, int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1);
  PTResult *ptr2_pts = findPTResult(ptr2);

  if (!ptr1_pts || !ptr2_pts)
    return false;

  PTResultIterator iter1(ptr1_pts, this);
  PTResultIterator iter2(ptr2_pts, this);

  if (iter1.size() != iter2.size() || iter1.size() == 0)
    return false;

  std::map<ObjectLocator *, path_cond_t, obj_loc_cmp> adjusted1, adjusted2;
  for (auto &item : iter1)
    adjusted1[item.first->offsetBy(offset1)] = item.second;
  for (auto &item : iter2)
    adjusted2[item.first->offsetBy(offset2)] = item.second;

  if (adjusted1.size() != adjusted2.size())
    return false;

  for (const auto &item : adjusted1) {
    ObjectLocator *loc = item.first;
    auto other = adjusted2.find(loc);
    if (other == adjusted2.end())
      return false;

    // Load-load matching is path-sensitive in Falcon: the reaching-condition
    // for each matched locator must also agree, not just the locator/version.
    if (item.second != other->second)
      return false;

    MemObject *obj = loc->getObj();
    if (obj->isNull() || obj->isUnknown())
      continue;

    if (loc->getVersion(pos1) != loc->getVersion(pos2))
      return false;
  }

  return true;
}

const PTGraph::GuardedPointsToSignature &
PTGraph::getGuardedPointsToSignature(PTResult *points_to, int64_t offset) {
  auto key = std::make_pair(points_to, offset);
  auto cached = guarded_points_to_signature_cache_.find(key);
  if (cached != guarded_points_to_signature_cache_.end() &&
      cached->second.revision == points_to->getRevision()) {
    return cached->second;
  }

  GuardedPointsToSignature signature;
  signature.revision = points_to->getRevision();
  signature.must_alias_eligible = true;
  std::map<ObjectLocator *, path_cond_t, obj_loc_cmp> adjusted;
  PTResultIterator iterator(points_to, this);
  for (const auto &item : iterator) {
    ObjectLocator *locator = item.first ? item.first->offsetBy(offset) : nullptr;
    if (!locator || !locator->getObj()->isValid() ||
        isUnknownOffset(locator->getOffset())) {
      signature.must_alias_eligible = false;
      continue;
    }
    auto inserted = adjusted.emplace(locator, item.second);
    if (!inserted.second) {
      inserted.first->second =
          findOrCreateOrRegion(inserted.first->second, item.second);
    }
  }
  if (adjusted.empty())
    signature.must_alias_eligible = false;

  size_t fingerprint = adjusted.size();
  for (const auto &item : adjusted) {
    signature.entries.push_back(item);
    const size_t locator_hash = std::hash<const void *>()(item.first);
    const size_t condition_hash = std::hash<const void *>()(item.second);
    fingerprint ^=
        locator_hash + 0x9e3779b9U + (fingerprint << 6) + (fingerprint >> 2);
    fingerprint ^=
        condition_hash + 0x9e3779b9U + (fingerprint << 6) + (fingerprint >> 2);
  }
  signature.fingerprint = fingerprint;

  if (cached == guarded_points_to_signature_cache_.end()) {
    return guarded_points_to_signature_cache_
        .emplace(key, std::move(signature))
        .first->second;
  }
  cached->second = std::move(signature);
  return cached->second;
}

path_cond_t PTGraph::getAliasCondition(Value *ptr1, Value *ptr2,
                                       int64_t offset1, int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1, false);
  PTResult *ptr2_pts = findPTResult(ptr2, false);
  if (!ptr1_pts || !ptr2_pts)
    return getFalseCond();

  const auto &lhs = getGuardedPointsToSignature(ptr1_pts, offset1);
  const auto &rhs = getGuardedPointsToSignature(ptr2_pts, offset2);
  auto lhs_item = lhs.entries.begin();
  auto rhs_item = rhs.entries.begin();
  obj_loc_cmp compare;
  path_cond_t result = getFalseCond();
  while (lhs_item != lhs.entries.end() && rhs_item != rhs.entries.end()) {
    if (compare(lhs_item->first, rhs_item->first)) {
      ++lhs_item;
      continue;
    }
    if (compare(rhs_item->first, lhs_item->first)) {
      ++rhs_item;
      continue;
    }

    path_cond_t overlap =
        findOrCreateAndRegion(lhs_item->second, rhs_item->second);
    if (isSatisfiable(overlap))
      result = findOrCreateOrRegion(result, overlap);
    ++lhs_item;
    ++rhs_item;
  }
  return result;
}

bool PTGraph::areMustAliases(Value *ptr1, Value *ptr2, int64_t offset1,
                             int64_t offset2) {
  PTResult *ptr1_pts = findPTResult(ptr1, false);
  PTResult *ptr2_pts = findPTResult(ptr2, false);
  if (!ptr1_pts || !ptr2_pts)
    return false;

  const auto &lhs = getGuardedPointsToSignature(ptr1_pts, offset1);
  const auto &rhs = getGuardedPointsToSignature(ptr2_pts, offset2);
  if (!lhs.must_alias_eligible || !rhs.must_alias_eligible ||
      lhs.fingerprint != rhs.fingerprint ||
      lhs.entries.size() != rhs.entries.size()) {
    return false;
  }

  // Confirm a hash match structurally so collisions cannot manufacture a
  // must-alias proof.
  for (size_t index = 0; index < lhs.entries.size(); ++index) {
    if (lhs.entries[index] != rhs.entries[index])
      return false;
  }
  return true;
}

void PTGraph::dumpMemObjs() {
  for (auto &pair : mem_objs) {
    outs() << "ID:" << pair.second << "\n";
    pair.first->dump();
  }
}

int PTGraph::getObjectToCallApDepth(MemObject *obj, CallInst *call) {
  if (!obj || !call)
    return FUNC_OBJ_UNREACHABLE;

  // Get or compute frontier
  set<MemObject *, mem_obj_cmp> &frontier = object_call_ap_depth_frontier[call];
  map<MemObject *, int, mem_obj_cmp> &cache =
      object_call_arg_ap_depth_cache[call];

  if (frontier.empty()) {
    // Initialize: add global objects
    for (MemObject *global_obj : global_objects) {
      frontier.insert(global_obj);
      cache[global_obj] = 1;
    }

    // Add objects reachable from call arguments
    for (unsigned i = 0; i < call->arg_size(); i++) {
      Value *arg = call->getArgOperand(i);
      PTResult *pts_result = findPTResult(arg, false);
      if (pts_result) {
        PTResultIterator result_iter(pts_result, this);
        for (auto &pt_item : result_iter) {
          MemObject *pt_obj = pt_item.first->getObj();
          if (!cache.count(pt_obj)) {
            cache[pt_obj] = 1;
            frontier.insert(pt_obj);
          }
        }
      }
    }
  }

  // Check cache
  auto cache_find = cache.find(obj);
  if (cache_find != cache.end())
    return cache_find->second;

  // Not in cache - compute on demand
  if (frontier.empty()) {
    cache[obj] = FUNC_OBJ_UNREACHABLE;
    return FUNC_OBJ_UNREACHABLE;
  }

  // Get frontier depth
  MemObject *frontier_sample = *frontier.begin();
  int frontier_depth = cache[frontier_sample];

  // Expand frontier until target found or max depth reached
  set<MemObject *, mem_obj_cmp> new_frontier;
  while (frontier_depth < lotus_restrict_obj_ap_depth) {
    for (MemObject *frontier_obj : frontier) {
      map<int64_t, Type *> &updated_offsets = frontier_obj->getUpdatedOffset();

      for (auto &offset_pair : updated_offsets) {
        int64_t offset = offset_pair.first;
        ObjectLocator *locator = frontier_obj->findLocator(offset, false);
        if (locator) {
          mem_value_t pt_values;
          locator->getValues(call, getEmptyCond(), pt_values);

          for (mem_value_item_t &value_item : pt_values) {
            Value *val = value_item.val;
            PTResult *pts_result = findPTResult(val, false);
            if (pts_result) {
              PTResultIterator result_iter(pts_result, this);
              for (auto &pt_item : result_iter) {
                MemObject *pt_obj = pt_item.first->getObj();
                if (!cache.count(pt_obj)) {
                  cache[pt_obj] = frontier_depth + 1;
                  new_frontier.insert(pt_obj);
                }
              }
            }
          }
        }
      }
    }

    frontier.clear();
    for (MemObject *mem_obj : new_frontier) {
      frontier.insert(mem_obj);
    }
    new_frontier.clear();

    // Check if target found
    cache_find = cache.find(obj);
    if (cache_find != cache.end())
      return cache_find->second;

    frontier_depth++;
  }

  cache[obj] = FUNC_OBJ_UNREACHABLE;
  return FUNC_OBJ_UNREACHABLE;
}

const DataLayout &PTGraph::getDL() { return lotus_aa->getDataLayout(); }

path_cond_t PTGraph::internCond(std::unique_ptr<PathCond> cond) {
  const auto &summary = cond->getConstraintSummary();
  auto it = formula_cond_cache_.find(summary);
  if (it != formula_cond_cache_.end())
    return it->second;

  path_cond_t raw = cond.get();
  formula_cond_cache_.emplace(summary, raw);
  cond_nodes_.push_back(std::move(cond));
  return raw;
}

path_cond_t PTGraph::getEmptyCond() {
  if (!true_cond_) {
    true_cond_ = internCond(std::unique_ptr<PathCond>(PathCond::createTrue()));
  }
  return true_cond_;
}

path_cond_t PTGraph::getFalseCond() {
  if (!false_cond_) {
    false_cond_ =
        internCond(std::unique_ptr<PathCond>(PathCond::createFalse()));
  }
  return false_cond_;
}

bool PTGraph::isAlwaysSatisfied(path_cond_t cond) const {
  if (!cond)
    return true;
  return cond->getConstraintSummary().always_true;
}

bool PTGraph::isSatisfiable(path_cond_t cond) const {
  if (!cond)
    return true;
  return !cond->getConstraintSummary().always_false;
}

bool PTGraph::isNoEffectFunction(Function *F) const {
  return lotus_aa && F && lotus_aa->getSpecManager().isNoEffect(F);
}

path_cond_t PTGraph::getValueCond(Value *value, bool sense) {
  if (!value)
    return sense ? getEmptyCond() : getFalseCond();

  if (auto *CI = dyn_cast<ConstantInt>(value)) {
    bool is_true = CI->isOne();
    return (is_true == sense) ? getEmptyCond() : getFalseCond();
  }

  auto key = make_pair(value, sense);
  auto it = value_cond_cache_.find(key);
  if (it != value_cond_cache_.end())
    return it->second;

  path_cond_t cond =
      internCond(std::unique_ptr<PathCond>(PathCond::createValueAtom(value, sense)));
  value_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::getBlockCond(BasicBlock *BB) { return getUnitRegion(BB); }

void PTGraph::buildControlDependenceInfo() {
  if (control_dep_ready_)
    return;

  control_dep_cache_.clear();
  unit_region_cache_.clear();

  if (lotus_aa) {
    if (gsa::ControlDependenceAnalysis *cda =
            lotus_aa->getControlDependenceAnalysis(analyzed_func)) {
      for (BasicBlock &BB : *analyzed_func) {
        if (!cda->isTracked(BB))
          continue;

        auto *target_node =
            post_dom_tree ? post_dom_tree->getNode(&BB) : nullptr;
        for (BasicBlock *controller : cda->getCDBlocks(&BB)) {
          path_cond_t combined_cond = nullptr;
          auto *controller_node =
              post_dom_tree ? post_dom_tree->getNode(controller) : nullptr;
          for (BasicBlock *succ : successors(controller)) {
            if (!succ)
              continue;

            path_cond_t edge_cond = getCFGEdgeCond(controller, succ);
            if (!isSatisfiable(edge_cond))
              continue;

            bool include_edge = false;
            if (target_node && controller_node) {
              if (post_dom_tree->dominates(controller, succ))
                continue;

              for (auto *succ_node = post_dom_tree->getNode(succ);
                   succ_node && succ_node != controller_node;
                   succ_node = succ_node->getIDom()) {
                if (succ_node == target_node) {
                  include_edge = true;
                  break;
                }
              }
            } else if (succ == &BB || cda->isReachable(succ, &BB)) {
              include_edge = true;
            }

            if (include_edge) {
              combined_cond = combined_cond
                                  ? findOrCreateOrRegion(combined_cond,
                                                         edge_cond)
                                  : edge_cond;
            }
          }

          if (combined_cond)
            control_dep_cache_[&BB][controller] = combined_cond;
        }
      }

      control_dep_ready_ = true;
      return;
    }
  }

  if (!post_dom_tree) {
    control_dep_ready_ = true;
    return;
  }

  for (BasicBlock &BB : *analyzed_func) {
    BasicBlock *controller = &BB;
    auto *controller_node = post_dom_tree->getNode(controller);
    if (!controller_node)
      continue;

    for (BasicBlock *succ : successors(controller)) {
      auto *succ_node = post_dom_tree->getNode(succ);
      if (!succ_node)
        continue;
      if (post_dom_tree->dominates(controller, succ))
        continue;

      path_cond_t edge_cond = getCFGEdgeCond(controller, succ);
      if (!isSatisfiable(edge_cond))
        continue;

      while (succ_node && succ_node != controller_node) {
        BasicBlock *curr = succ_node->getBlock();
        path_cond_t &curr_cond = control_dep_cache_[curr][controller];
        curr_cond =
            curr_cond ? findOrCreateOrRegion(curr_cond, edge_cond) : edge_cond;
        succ_node = succ_node->getIDom();
      }
    }
  }

  control_dep_ready_ = true;
}

path_cond_t PTGraph::getCFGEdgeCond(BasicBlock *src_bb, BasicBlock *succ_bb) {
  if (!src_bb || !succ_bb)
    return getFalseCond();

  auto key = make_pair(src_bb, succ_bb);
  auto it = edge_cond_cache_.find(key);
  if (it != edge_cond_cache_.end())
    return it->second;

  path_cond_t cond = getEmptyCond();
  Instruction *term = src_bb->getTerminator();
  if (auto *br = dyn_cast<BranchInst>(term)) {
    if (br->isConditional()) {
      bool sense = succ_bb == br->getSuccessor(0);
      cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createBranchAtom(src_bb, succ_bb, br->getCondition(), sense)));
    }
  } else if (auto *sw = dyn_cast<SwitchInst>(term)) {
    path_cond_t edge_cond = nullptr;
    for (const auto &case_it : sw->cases()) {
      if (case_it.getCaseSuccessor() != succ_bb)
        continue;
      path_cond_t case_cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createSwitchCaseAtom(src_bb, succ_bb, sw->getCondition(),
                                         case_it.getCaseValue())));
      edge_cond =
          edge_cond ? findOrCreateOrRegion(edge_cond, case_cond) : case_cond;
    }

    if (sw->getDefaultDest() == succ_bb) {
      path_cond_t default_cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createSwitchDefaultAtom(src_bb, succ_bb,
                                            sw->getCondition())));
      edge_cond = edge_cond ? findOrCreateOrRegion(edge_cond, default_cond)
                            : default_cond;
    }

    cond = edge_cond ? edge_cond : getFalseCond();
  } else if (auto *invoke = dyn_cast<InvokeInst>(term)) {
    if (invoke->getNormalDest() == succ_bb) {
      cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createInvokeNormalAtom(src_bb, succ_bb)));
    } else if (invoke->getUnwindDest() == succ_bb) {
      cond = internCond(std::unique_ptr<PathCond>(
          PathCond::createInvokeUnwindAtom(src_bb, succ_bb)));
    } else {
      cond = getFalseCond();
    }
  }

  edge_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::localizePathCond(path_cond_t cond) {
  if (!cond)
    return getEmptyCond();
  if (cond->getOwnerFunc() == nullptr || cond->getOwnerFunc() == analyzed_func)
    return cond;
  return importPathCond(cond, nullptr, nullptr);
}

path_cond_t PTGraph::getComplementaryBranchCond(path_cond_t cond) {
  if (!cond || cond->getKind() != PathCond::Kind::BranchAtom)
    return nullptr;

  BasicBlock *block = cond->getBlock();
  BasicBlock *succ = cond->getSuccessor();
  auto *br =
      block ? dyn_cast_or_null<BranchInst>(block->getTerminator()) : nullptr;
  if (!br || !br->isConditional() || br->getNumSuccessors() != 2)
    return nullptr;

  BasicBlock *other_succ =
      (succ == br->getSuccessor(0)) ? br->getSuccessor(1) : br->getSuccessor(0);
  if (!other_succ || other_succ == succ)
    return nullptr;
  return getCFGEdgeCond(block, other_succ);
}

path_cond_t PTGraph::getUnitRegion(BasicBlock *BB) {
  if (!BB)
    return getEmptyCond();

  buildControlDependenceInfo();

  auto cache_it = unit_region_cache_.find(BB);
  if (cache_it != unit_region_cache_.end())
    return cache_it->second;

  path_cond_t result = getEmptyCond();
  auto dep_it = control_dep_cache_.find(BB);
  if (dep_it != control_dep_cache_.end()) {
    bool has_dep = false;
    for (auto &dep_item : dep_it->second) {
      path_cond_t dep_region = getUnitRegion(dep_item.first);
      path_cond_t dep_cond = findOrCreateAndRegion(dep_region, dep_item.second);
      result = has_dep ? findOrCreateOrRegion(result, dep_cond) : dep_cond;
      has_dep = true;
    }
  }

  unit_region_cache_[BB] = result;
  return result;
}

path_cond_t PTGraph::findOrCreateBBRegion(BasicBlock *src_bb,
                                          BasicBlock *target_bb) {
  if (!src_bb || !target_bb)
    return getFalseCond();
  if (src_bb == target_bb)
    return getEmptyCond();

  auto src_it = bb_region_cache.find(src_bb);
  if (src_it != bb_region_cache.end()) {
    auto target_it = src_it->second.find(target_bb);
    if (target_it != src_it->second.end())
      return target_it->second;
  }

  if (dom_tree && !dom_tree->dominates(src_bb, target_bb)) {
    bb_region_cache[src_bb][target_bb] = getFalseCond();
    return getFalseCond();
  }

  buildControlDependenceInfo();

  path_cond_t result = getEmptyCond();
  bool has_dep = false;
  auto dep_it = control_dep_cache_.find(target_bb);
  if (dep_it != control_dep_cache_.end()) {
    for (auto &dep_item : dep_it->second) {
      BasicBlock *dep_bb = dep_item.first;
      if (dep_bb != src_bb && dom_tree && !dom_tree->dominates(src_bb, dep_bb))
        continue;

      path_cond_t dep_region = (dep_bb == src_bb)
                                   ? getEmptyCond()
                                   : findOrCreateBBRegion(src_bb, dep_bb);
      path_cond_t dep_cond = findOrCreateAndRegion(dep_region, dep_item.second);
      result = has_dep ? findOrCreateOrRegion(result, dep_cond) : dep_cond;
      has_dep = true;
    }
  }

  bb_region_cache[src_bb][target_bb] = result;
  return result;
}

path_cond_t PTGraph::importPathCond(path_cond_t cond, Value *callsite,
                                    Function *callee) {
  if (!cond)
    return getEmptyCond();
  if (cond->getOwnerFunc() == analyzed_func || cond->getOwnerFunc() == nullptr)
    return cond;
  if (isAlwaysSatisfied(cond))
    return getEmptyCond();
  if (!isSatisfiable(cond))
    return getFalseCond();

  auto it = imported_cond_cache_.find(cond);
  if (it != imported_cond_cache_.end())
    return it->second;

  path_cond_t imported = internCond(std::unique_ptr<PathCond>(
      PathCond::createImportedAtom(analyzed_func, cond)));
  imported_cond_cache_[cond] = imported;
  return imported;
}

path_cond_t PTGraph::getCallTargetCond(Value *called_value, Function *callee) {
  if (!called_value || !callee)
    return getEmptyCond();

  auto key = make_pair(called_value, callee);
  auto it = call_target_cond_cache_.find(key);
  if (it != call_target_cond_cache_.end())
    return it->second;

  path_cond_t cond = internCond(std::unique_ptr<PathCond>(
      PathCond::createCallTargetAtom(called_value, callee)));
  call_target_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::findOrCreateAndRegion(path_cond_t lhs, path_cond_t rhs) {
  if (!lhs)
    lhs = getEmptyCond();
  if (!rhs)
    rhs = getEmptyCond();
  lhs = localizePathCond(lhs);
  rhs = localizePathCond(rhs);
  if (!isSatisfiable(lhs) || !isSatisfiable(rhs))
    return getFalseCond();
  if (isAlwaysSatisfied(lhs))
    return rhs;
  if (isAlwaysSatisfied(rhs))
    return lhs;
  if (lhs == rhs)
    return lhs;

  auto key = canonicalCondPair(lhs, rhs);
  auto it = and_cond_cache_.find(key);
  if (it != and_cond_cache_.end())
    return it->second;

  path_cond_t cond =
      internCond(std::unique_ptr<PathCond>(PathCond::createAnd(key.first, key.second)));
  and_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::findOrCreateOrRegion(path_cond_t lhs, path_cond_t rhs) {
  if (!lhs)
    lhs = getEmptyCond();
  if (!rhs)
    rhs = getEmptyCond();
  lhs = localizePathCond(lhs);
  rhs = localizePathCond(rhs);
  if (isAlwaysSatisfied(lhs) || isAlwaysSatisfied(rhs))
    return getEmptyCond();
  if (!isSatisfiable(lhs))
    return rhs;
  if (!isSatisfiable(rhs))
    return lhs;
  if (lhs == rhs)
    return lhs;

  auto key = canonicalCondPair(lhs, rhs);
  auto it = or_cond_cache_.find(key);
  if (it != or_cond_cache_.end())
    return it->second;

  path_cond_t cond =
      internCond(std::unique_ptr<PathCond>(PathCond::createOr(key.first, key.second)));
  or_cond_cache_[key] = cond;
  return cond;
}

path_cond_t PTGraph::findOrCreateNotRegion(path_cond_t cond) {
  if (!cond)
    return getFalseCond();
  cond = localizePathCond(cond);
  if (isAlwaysSatisfied(cond))
    return getFalseCond();
  if (!isSatisfiable(cond))
    return getEmptyCond();
  if (cond->getKind() == PathCond::Kind::ValueAtom)
    return getValueCond(cond->getValue(), !cond->getSense());
  if (cond->getKind() == PathCond::Kind::BranchAtom) {
    if (path_cond_t complement = getComplementaryBranchCond(cond))
      return complement;
  }
  if (cond->getKind() == PathCond::Kind::Not)
    return cond->getLhs() ? cond->getLhs() : getEmptyCond();

  auto it = not_cond_cache_.find(cond);
  if (it != not_cond_cache_.end())
    return it->second;

  path_cond_t neg =
      internCond(std::unique_ptr<PathCond>(PathCond::createNot(cond)));
  not_cond_cache_[cond] = neg;
  return neg;
}

path_cond_t PTGraph::findOrCreateUnitPhiRegion(BasicBlock *cur_bb,
                                               BasicBlock *incoming_bb) {
  auto cache_it = phi_region_cache.find(cur_bb);
  if (cache_it != phi_region_cache.end()) {
    auto cond_it = cache_it->second.find(incoming_bb);
    if (cond_it != cache_it->second.end())
      return cond_it->second;
  }

  path_cond_t cond = getEmptyCond();
  if (incoming_bb) {
    if (IntraLotusAAConfig::lotus_use_full_phi_cond) {
      cond = getUnitRegion(incoming_bb);
    } else {
      BasicBlock *dom_bb = nullptr;
      if (dom_tree) {
        if (DomTreeNode *sink_node = dom_tree->getNode(cur_bb)) {
          if (DomTreeNode *idom_node = sink_node->getIDom())
            dom_bb = idom_node->getBlock();
        }
      }

      cond = dom_bb ? findOrCreateBBRegion(dom_bb, incoming_bb)
                    : getUnitRegion(incoming_bb);
    }

    if (auto *br = dyn_cast_or_null<BranchInst>(incoming_bb->getTerminator())) {
      if (br->isConditional())
        cond = findOrCreateAndRegion(cond, getCFGEdgeCond(incoming_bb, cur_bb));
    }
  }
  phi_region_cache[cur_bb][incoming_bb] = cond;
  return cond;
}
