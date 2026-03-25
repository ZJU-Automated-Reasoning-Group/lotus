/// @file ObjectLocator.cpp
/// @brief Field-sensitive memory location tracking with flow-sensitive value
/// storage
///
/// This file implements `ObjectLocator`, which tracks **values stored at
/// specific memory locations** (object + offset) across program execution
/// paths. This is the key component enabling **flow-sensitive** and
/// **field-sensitive** analysis.
///
/// **Core Abstraction:**
/// ```
/// ObjectLocator = (MemObject, offset)
/// Represents a specific field/location within a memory object
/// ```
///
/// **Flow-Sensitive Value Tracking:**
/// Values are tracked **per basic block** using SSA-style versioning:
/// ```
/// Entry:  loc.value = ⊥
/// BB1:    store v1 → loc     // loc.value[BB1] = {v1}
/// BB2:    store v2 → loc     // loc.value[BB2] = {v2}
/// BB3 (join of BB1,BB2):     // loc.value[BB3] = {v1,v2} (via phi)
/// ```
///
/// **Update Semantics:**
///
/// 1. **Strong Update** (must-point):
///    - Overwrites previous value completely
///    - Used when pointer provably points to single location
///    - Example: `x = 5; x = 10;` → x is 10 (not {5,10})
///
/// 2. **Weak Update** (may-point):
///    - Merges with previous values
///    - Used when pointer may point to multiple locations
///    - Propagated via SSA-style φ-placement using dominance frontiers
///
/// **SSA-Style Φ-Placement:**
/// Stores trigger φ-placement in dominance frontiers:
/// ```
/// BB1: store v1, loc        // Define loc in BB1
/// BB2: store v2, loc        // Define loc in BB2
/// BB3 = join(BB1, BB2):     // Auto-place weak φ(v1, v2)
/// ```
/// Uses LLVM's `IteratedDominanceFrontier` (modern LLVM 14+)
///
/// **Value Versioning:**
/// ```
/// getValues(from_inst, result):
///   Walk dominator tree from from_inst upward
///   Collect all reaching definitions
///   Stop at strong updates (must-alias)
///   Continue through weak updates (may-alias)
/// ```
///
/// **Configuration Limits** (heuristics for scalability):
/// - `lotus_memory_max_bb_load`: Max values per BB (default: 1000)
/// - `lotus_memory_max_bb_depth`: Max BB depth to search (default: 50)
/// - `lotus_memory_max_load`: Max total values (default: 5000)
/// - `lotus_memory_store_depth`: Max φ-placement depth (default: 100)
///
/// **Global Constant Handling:**
/// Constant globals return their initializer value directly (optimization).
///
/// **Pseudo-Argument Creation:**
/// Symbolic objects may create pseudo-arguments for undefined field accesses,
/// enabling inter-procedural side-effect tracking.
///
/// @see MemObject for object-level abstraction
/// @see storeValue() for update logic (strong vs weak)
/// @see getValues() for flow-sensitive value retrieval
/// @see placePhi() for SSA-style φ-placement algorithm

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/LotusAA/MemoryModel/MemObject.h"
#include "Alias/LotusAA/MemoryModel/PointsToGraph.h"
#include "Alias/LotusAA/Support/LotusConfig.h"

#include <llvm/Analysis/IteratedDominanceFrontier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

// Command-line options for memory tracking limits
static cl::opt<int> lotus_memory_max_bb_load(
    "lotus-restrict-memory-max-bb-load",
    cl::desc("Maximum values read from memory location per BB"),
    cl::init(LotusConfig::MemoryLimits::DEFAULT_MAX_BB_LOAD), cl::Hidden);

static cl::opt<int> lotus_memory_max_bb_depth(
    "lotus-restrict-memory-max-bb-depth",
    cl::desc("Maximum dominating basic blocks to track"),
    cl::init(LotusConfig::MemoryLimits::DEFAULT_MAX_BB_DEPTH), cl::Hidden);

static cl::opt<int> lotus_memory_max_load(
    "lotus-restrict-memory-max-load",
    cl::desc("Maximum values read from memory location total"),
    cl::init(LotusConfig::MemoryLimits::DEFAULT_MAX_LOAD), cl::Hidden);

static cl::opt<int> lotus_memory_store_depth(
    "lotus-restrict-memory-store-depth",
    cl::desc("Maximum BBs to track for store operations"),
    cl::init(LotusConfig::MemoryLimits::DEFAULT_STORE_DEPTH), cl::Hidden);

static int getLotusMemoryMaxPassingFunc() {
  return IntraLotusAAConfig::lotus_memory_max_passing_func;
}

//===----------------------------------------------------------------------===//
// LocValue Implementation
//===----------------------------------------------------------------------===//

Value *LocValue::FREE_VARIABLE = nullptr;
Value *LocValue::NO_VALUE = nullptr;
Value *LocValue::UNDEF_VALUE = nullptr;
Value *LocValue::SUMMARY_VALUE = nullptr;

void LocValue::dump() {
  outs() << "    Value: ";
  if (val == FREE_VARIABLE)
    outs() << "FREE";
  else if (val == NO_VALUE)
    outs() << "NO_VALUE";
  else if (val == UNDEF_VALUE)
    outs() << "UNDEF";
  else if (val == SUMMARY_VALUE)
    outs() << "SUMMARY";
  else if (val->hasName())
    outs() << val->getName();
  else
    val->print(outs());

  outs() << " cond=";
  if (!cond)
    outs() << "<null>";
  else
    cond->print(outs());

  outs() << " @";
  if (pos_inst && pos_inst->getParent())
    outs() << pos_inst->getParent()->getName();
  else
    outs() << "entry";

  outs() << (isStrongUpdate() ? " [STRONG]\n" : " [WEAK]\n");
}

//===----------------------------------------------------------------------===//
// ObjectLocator Implementation
//===----------------------------------------------------------------------===//

ObjectLocator::ObjectLocator(MemObject *obj, int64_t off)
    : object(obj), offset(off), load_level(FUNC_LEVEL_UNDEFINED),
      store_level(FUNC_LEVEL_UNDEFINED), obj_index(obj->obj_index),
      loc_index(obj->loc_index++) {}

ObjectLocator::ObjectLocator(const ObjectLocator &locator)
    : object(locator.getObj()), offset(locator.getOffset()),
      load_level(locator.load_level), store_level(locator.store_level),
      obj_index(locator.getObj()->obj_index),
      loc_index(locator.getObj()->loc_index++) {}

ObjectLocator::~ObjectLocator() {
  for (auto &it : loc_values) {
    for (auto *lv : it.second)
      delete lv;
  }
}

PTGraph *ObjectLocator::getPTG() { return object->getPTG(); }

ObjectLocator *ObjectLocator::offsetBy(int64_t extra_off) {
  return object->findLocator(PTGraph::composeOffset(offset, extra_off), true);
}

void ObjectLocator::dump() {
  for (auto &bb_vals : loc_values) {
    outs() << "  BB " << bb_vals.first->getName() << ":\n";
    for (LocValue *lv : bb_vals.second) {
      lv->dump();
    }
  }
}

std::vector<LocValue *> *ObjectLocator::getValueList(BasicBlock *bb) {
  auto it = loc_values.find(bb);
  return (it == loc_values.end()) ? nullptr : &(it->second);
}

LocValue *ObjectLocator::storeValue(Value *val, Instruction *source,
                                    path_cond_t cond, int function_level) {
  if (object->isNull() || object->isUnknown())
    return nullptr;

  if (val != LocValue::FREE_VARIABLE && val != LocValue::NO_VALUE &&
      val->getType()->isAggregateType() && (!val->getType()->isPointerTy()))
    return nullptr;

  if (store_level == FUNC_LEVEL_UNDEFINED || store_level > function_level) {
    if (function_level != FUNC_LEVEL_UNDEFINED)
      store_level = function_level;
  }

  BasicBlock *src_bb = source->getParent();
  LocValue::UpdateType update_type =
      getPTG()->isAlwaysSatisfied(cond) ? LocValue::STRONG : LocValue::WEAK;

  for (auto *loc_val : loc_values[src_bb]) {
    if (loc_val->getPos() == source && loc_val->getVal() == val) {
      path_cond_t pre_cond = loc_val->getCond();
      loc_val->setCond(getPTG()->findOrCreateOrRegion(pre_cond, cond));
      if (loc_val->isStrongUpdate() || update_type == LocValue::STRONG)
        loc_val->resetUpdateType(LocValue::STRONG);
      return loc_val;
    }
  }

  LocValue *loc_val = new LocValue(val, source, cond, update_type);
  loc_values[src_bb].push_back(loc_val);
  placePhi(loc_val, src_bb);

  if (val != LocValue::FREE_VARIABLE) {
    Type *val_type = val->getType();
    if (val_type->isVoidTy())
      val_type = Type::getInt8Ty(val->getContext());
    object->getUpdatedOffset()[offset] = val_type;
    object->getStoredValues()[offset].insert(val);

    if (val != LocValue::NO_VALUE && isa<PointerType>(val_type)) {
      object->getPointerOffset()[offset] = val_type;
    }
  }

  return loc_val;
}

void ObjectLocator::placePhi(LocValue *loc_value, BasicBlock *bb_start) {
  PTGraph *pt_graph = getPTG();
  DominatorTree *dom_tree = pt_graph->getDomTree();

  if (!dom_tree)
    return;

  // Use modern IDFCalculator (LLVM 14+) to compute iterated dominance frontier
  // This replaces the old DominanceFrontierWrapperPass which was removed in
  // LLVM 12+
  SmallVector<BasicBlock *, 32> Frontier;
  SmallPtrSet<BasicBlock *, 32> DefBlocks;
  DefBlocks.insert(bb_start);

  ForwardIDFCalculator IDF(*dom_tree);
  IDF.setDefiningBlocks(DefBlocks);
  IDF.calculate(Frontier);

  // Apply depth limit
  int num_blocks = Frontier.size();
  if (lotus_memory_store_depth != -1 && num_blocks > lotus_memory_store_depth) {
    num_blocks = lotus_memory_store_depth;
  }

  for (int i = 0; i < num_blocks; i++) {
    BasicBlock *processing_bb = Frontier[i];
    path_cond_t phi_cond = pt_graph->getUnitRegion(bb_start);
    path_cond_t processing_cond =
        pt_graph->findOrCreateAndRegion(loc_value->getCond(), phi_cond);
    LocValue *phi_lv = new LocValue(loc_value->getVal(), loc_value->getPos(),
                                    processing_cond, LocValue::WEAK);
    loc_values[processing_bb].push_back(phi_lv);
  }
}

LocValue *ObjectLocator::getVersion(Instruction *pos_inst) {
  DominatorTree *DT = getPTG()->getDomTree();
  if (!DT)
    return nullptr;

  BasicBlock *bb = pos_inst->getParent();
  BasicBlock *startBB = bb;

  while (bb) {
    std::vector<LocValue *> *lv_list = getValueList(bb);

    if (lv_list) {
      int end_pos = (int)lv_list->size();

      if (bb == startBB) {
        auto it = bb->rbegin(), ie = bb->rend();

        while (end_pos > 0) {
          Instruction *last_loc = lv_list->at(end_pos - 1)->getPos();
          Instruction *inst = nullptr;

          for (; it != ie; ++it) {
            inst = &(*it);
            if (inst == pos_inst || inst == last_loc)
              break;
          }

          if (inst == pos_inst)
            break;

          --end_pos;
        }
      }

      if (end_pos != 0)
        return lv_list->at(end_pos - 1);
    }

    // `DominatorTree` does not include unreachable blocks. Also the entry block
    // has no IDom. Avoid null-dereferences and simply stop the walk.
    DomTreeNode *node = DT->getNode(bb);
    if (!node)
      break;
    DomTreeNode *idom = node->getIDom();
    bb = idom ? idom->getBlock() : nullptr;
  }

  return nullptr;
}

// Forward declaration for helper
static Value *get_constant_from_aggregate(Constant *val, int64_t offset,
                                          const DataLayout *DL);

Argument *ObjectLocator::getValues(Instruction *from_loc, path_cond_t pre_cond,
                                   mem_value_t &res, Type *symbol_type,
                                   int function_level,
                                   bool enable_strong_update,
                                   ObjectLocator *func_call_cache,
                                   bool is_include_func_summary) {
  if (!pre_cond)
    pre_cond = getPTG()->getEmptyCond();

  // Check for constant global initializer
  Value *alloc_site = object->getAllocSite();
  if (alloc_site) {
    if (GlobalVariable *gv = dyn_cast<GlobalVariable>(alloc_site)) {
      if (gv->isConstant()) {
        Value *constant_global = getInitializerForGlobalValue();
        if (constant_global) {
          res.push_back(
              mem_value_item_t(pre_cond, nullptr, constant_global, 1.0f));
          return nullptr;
        } else {
          return nullptr; // Constant with no initializer
        }
      }
    }
  }

  mem_value_t *possible_updating_callsite =
      (is_include_func_summary &&
       IntraLotusAAConfig::lotus_enable_summary_value)
          ? &res
          : nullptr;

  mem_value_t func_in_path;
  if (func_call_cache) {
    func_call_cache->getValues(from_loc, pre_cond, func_in_path, nullptr,
                               FUNC_LEVEL_UNDEFINED, false);
  }

  size_t func_idx = 0;

  DominatorTree *DT = getPTG()->getDomTree();
  if (!DT)
    return nullptr;

  auto get_confidence_with_no_call_barrier =
      [&](int val_seq, path_cond_t current_pre_cond,
          mem_value_t *func_detailed_res) -> float {
    float result = 1.0f;
    int max_passing_func = getLotusMemoryMaxPassingFunc();

    if (val_seq == PTGraph::VALUE_SEQ_UNDEF)
      return result;

    while (func_idx < func_in_path.size()) {
      if (max_passing_func != -1 &&
          static_cast<int>(func_idx) > max_passing_func) {
        return 0.1f;
      }

      mem_value_item_t &mem_item = func_in_path[func_idx];
      Value *func_val = mem_item.pos;
      int func_seq = getPTG()->getSequenceNum(func_val);
      if (val_seq == PTGraph::VALUE_SEQ_INFINITE || func_seq <= val_seq)
        break;

      ++func_idx;

      if (func_seq == PTGraph::VALUE_SEQ_UNDEF)
        continue;

      CallBase *call = dyn_cast<CallBase>(func_val);
      if (!call)
        continue;

      if (Function *func = call->getCalledFunction()) {
        if (getPTG()->isNoEffectFunction(func))
          continue;
      }

      path_cond_t func_cond = mem_item.cond;
      float func_exec_confidence = 1.0f;
      if (!getPTG()->isAlwaysSatisfied(func_cond)) {
        path_cond_t func_final_cond =
            getPTG()->findOrCreateAndRegion(current_pre_cond, func_cond);
        if (!getPTG()->isSatisfiable(func_final_cond)) {
          func_exec_confidence = 0.0f;
        } else {
          func_exec_confidence =
              llvm::LotusConfig::Heuristics::COND_SAT_PROBABILITY;
        }
      }

      int ap_depth = PTGraph::FUNC_OBJ_UNREACHABLE;
      if (CallInst *call_inst = dyn_cast<CallInst>(call)) {
        ap_depth = getPTG()->getObjectToCallApDepth(getObj(), call_inst);
      }
      if (ap_depth == PTGraph::FUNC_OBJ_UNREACHABLE)
        continue;

      if (Function *callee = call->getCalledFunction()) {
        if (PTGraph *callee_graph = getPTG()->getPtGraph(callee)) {
          if (callee_graph->getInlineApDepth() >= ap_depth)
            continue;
        }
      }

      float func_affect_confidence =
          llvm::LotusConfig::Heuristics::MEM_CHANGE_PROBABILITY[ap_depth] *
          func_exec_confidence;
      result *= (1.0f - func_affect_confidence);

      if (func_detailed_res && IntraLotusAAConfig::lotus_enable_summary_value) {
        if (Instruction *func_inst = dyn_cast<Instruction>(func_val)) {
          func_detailed_res->push_back(mem_value_item_t(
              getPTG()->findOrCreateAndRegion(current_pre_cond, func_cond),
              func_inst, LocValue::SUMMARY_VALUE, func_affect_confidence));
        }
      }
    }

    return result;
  };

  path_cond_t curr_anti_cond = getPTG()->getEmptyCond();
  float curr_confidence = 1.0f;

  BasicBlock *bb = from_loc->getParent();
  BasicBlock *startBB = bb;

  int bb_tracked = 0;
  int value_loaded = 0;

  // Climb dominator tree collecting values
  while (bb) {
    std::vector<LocValue *> *lv_list = getValueList(bb);

    if (lv_list) {
      int end_pos = lv_list->size();

      // Apply heuristic limits when strong update enabled
      if (enable_strong_update) {
        if (lotus_memory_max_bb_depth != -1 &&
            bb_tracked > lotus_memory_max_bb_depth)
          return nullptr;

        if (lotus_memory_max_bb_load != -1 &&
            end_pos > lotus_memory_max_bb_load)
          return nullptr;

        if (lotus_memory_max_load != -1 && value_loaded > lotus_memory_max_load)
          return nullptr;
      }

      if (bb == startBB) {
        // Find last locator value before or equal to from_loc
        auto it = bb->rbegin(), ie = bb->rend();
        while (end_pos > 0) {
          Instruction *last_loc = lv_list->at(end_pos - 1)->getPos();
          Instruction *inst = nullptr;
          for (; it != ie; ++it) {
            inst = &(*it);
            if (inst == from_loc || inst == last_loc) {
              // Skip stored values to same instruction
              while (inst == last_loc && end_pos >= 1) {
                --end_pos;
                if (end_pos < 1)
                  break;
                last_loc = lv_list->at(end_pos - 1)->getPos();
              }
              break;
            }
          }
          if (inst == from_loc)
            break;
          --end_pos;
        }
      }

      // Collect values from this BB
      for (int i = end_pos - 1; i >= 0; --i) {
        LocValue *curr_lv = lv_list->at(i);
        path_cond_t cond = curr_lv->getCond();
        path_cond_t mem_val_cond =
            getPTG()->findOrCreateAndRegion(curr_anti_cond, cond);
        Value *val = curr_lv->getVal();
        Instruction *pos = curr_lv->getPos();
        path_cond_t final_cond =
            getPTG()->findOrCreateAndRegion(pre_cond, mem_val_cond);

        if (getPTG()->isSatisfiable(final_cond) && val != LocValue::NO_VALUE) {
          float no_effect_confidence = 1.0f;
          if (IntraLotusAAConfig::lotus_enable_score_computation) {
            int val_seq = pos ? getPTG()->getSequenceNum(pos)
                              : PTGraph::VALUE_SEQ_INFINITE;
            no_effect_confidence = get_confidence_with_no_call_barrier(
                val_seq, final_cond, possible_updating_callsite);
          }

          curr_confidence *= no_effect_confidence;
          res.push_back(
              mem_value_item_t(final_cond, pos, val, curr_confidence));
          value_loaded++;
        }

        if (enable_strong_update) {
          if (curr_lv->isStrongUpdate()) {
            // Stop at strong update
            return nullptr;
          }
          path_cond_t anti_cond = getPTG()->findOrCreateNotRegion(cond);
          curr_anti_cond =
              getPTG()->findOrCreateAndRegion(curr_anti_cond, anti_cond);
        }
      }
    }

    // Move to immediate dominator.
    // Note: unreachable blocks are not in the DT, and the entry block has no
    // IDom.
    DomTreeNode *node = DT->getNode(bb);
    if (!node)
      break;
    DomTreeNode *idom_node = node->getIDom();
    bb = idom_node ? idom_node->getBlock() : nullptr;
    bb_tracked++;
  }

  path_cond_t default_cond =
      getPTG()->findOrCreateAndRegion(curr_anti_cond, pre_cond);
  if (!getPTG()->isSatisfiable(default_cond))
    return nullptr;

  if (IntraLotusAAConfig::lotus_enable_score_computation) {
    float no_effect_confidence = get_confidence_with_no_call_barrier(
        0, default_cond, possible_updating_callsite);
    curr_confidence *= no_effect_confidence;
  }

  // No explicit stores found - check what to return
  if (SymbolicMemObject *sym_obj = dyn_cast<SymbolicMemObject>(object)) {
    // Symbolic object - create pseudo-argument
    Function *base_func = object->getPTG()->getFunc();
    if (base_func->getName() == "main") {
      // Main function - try to get global initializer
      Value *init_val = getInitializerForGlobalValue();
      if (init_val) {
        res.push_back(
            mem_value_item_t(default_cond, nullptr, init_val, curr_confidence));
        return nullptr;
      }
    } else {
      // Non-main function - create pseudo-argument
      Argument *pseudo_arg = sym_obj->findCreatePseudoArg(this, symbol_type);
      if (pseudo_arg) {
        if (load_level == FUNC_LEVEL_UNDEFINED || load_level > function_level) {
          if (function_level != FUNC_LEVEL_UNDEFINED)
            load_level = function_level;
        }
        res.push_back(mem_value_item_t(default_cond, nullptr, pseudo_arg,
                                       curr_confidence));
        return pseudo_arg;
      }
    }
  }

  // Return appropriate default value
  if (object->isReallyAllocated()) {
    res.push_back(mem_value_item_t(default_cond, nullptr, LocValue::UNDEF_VALUE,
                                   curr_confidence));
  } else {
    res.push_back(mem_value_item_t(default_cond, nullptr,
                                   LocValue::FREE_VARIABLE, curr_confidence));
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
// Constant Extraction Helper
//===----------------------------------------------------------------------===//

static Value *get_constant_from_aggregate(Constant *val, int64_t offset,
                                          const DataLayout *DL) {
  if (!val)
    return nullptr;

  Value *result = nullptr;

  if (ConstantDataArray *array_global = dyn_cast<ConstantDataArray>(val)) {
    Type *elem_type = array_global->getElementType();
    int64_t element_size = (int64_t)DL->getTypeSizeInBits(elem_type);
    if (element_size != 0) {
      int64_t idx = offset / element_size;
      int64_t remainder = offset % element_size;
      if (idx >= 0 && remainder >= 0 &&
          (uint64_t)idx < array_global->getNumElements()) {
        Constant *element = array_global->getElementAsConstant((unsigned)idx);
        result = get_constant_from_aggregate(element, remainder, DL);
      }
    }
  } else if (ConstantArray *array_global = dyn_cast<ConstantArray>(val)) {
    Type *elem_type = array_global->getType()->getElementType();
    int64_t element_size = (int64_t)DL->getTypeSizeInBits(elem_type);
    if (element_size != 0) {
      int64_t idx = offset / element_size;
      int64_t remainder = offset % element_size;
      if (idx >= 0 && remainder >= 0 &&
          (uint64_t)idx < array_global->getType()->getNumElements()) {
        result = get_constant_from_aggregate(
            array_global->getAggregateElement((unsigned)idx), remainder, DL);
      }
    }
  } else if (ConstantStruct *struct_global = dyn_cast<ConstantStruct>(val)) {
    StructType *st = struct_global->getType();
    int n_elem = st->getNumContainedTypes();
    int cur_size = 0;
    int last_size = 0;
    int idx;
    for (idx = 0; idx < n_elem; idx++) {
      if (cur_size >= offset) {
        break;
      }

      Type *t = st->getContainedType(idx);
      last_size = cur_size;
      cur_size += (int)DL->getTypeSizeInBits(t);
    }

    if (cur_size == offset && idx < n_elem) {
      Constant *elem_val = struct_global->getAggregateElement(idx);
      result = get_constant_from_aggregate(elem_val, 0, DL);
    } else if (cur_size > offset && last_size < offset) {
      Constant *elem_val = struct_global->getAggregateElement(idx - 1);
      result = get_constant_from_aggregate(elem_val, offset - last_size, DL);
    }
  } else {
    // Scalar constant
    if (offset == 0)
      result = val;
  }

  // Strip casts from result
  if (result) {
    if (CastInst *cast_result = dyn_cast<CastInst>(result)) {
      result = cast_result->getOperand(0);
    }
    if (ConstantExpr *const_expr = dyn_cast<ConstantExpr>(result)) {
      if (Instruction::isCast(const_expr->getOpcode())) {
        result = const_expr->getOperand(0);
      }
    }
  }

  return result;
}

Value *ObjectLocator::getInitializerForGlobalValue() {
  GlobalVariable *gv = dyn_cast_or_null<GlobalVariable>(object->getAllocSite());
  if (!gv || !gv->hasInitializer())
    return nullptr;

  Constant *init_val = gv->getInitializer();
  const DataLayout *DL = &getPTG()->getDL();
  return get_constant_from_aggregate(init_val, getOffset(), DL);
}

//===----------------------------------------------------------------------===//
// Pretty Printing
//===----------------------------------------------------------------------===//

namespace llvm {

raw_ostream &operator<<(raw_ostream &out, ObjectLocator &locator) {
  out << "[" << locator.getObj()->getName() << "].";
  if (PTGraph::isUnknownOffset(locator.getOffset()))
    out << "unknown";
  else
    out << locator.getOffset();
  return out;
}

} // namespace llvm
