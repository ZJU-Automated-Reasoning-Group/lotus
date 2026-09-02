/// @file Utilities.cpp
/// @brief Helper utilities for intra-procedural analysis
///
/// Provides utility functions for `IntraLotusAA` including:
///
/// **Memory Management:**
/// - `clearIntermediatePtsResult()`: Clear temporary pointer analysis data
/// - `clearIntermediateCgResult()`: Clear temporary call graph data
/// - `clearMemObjectResult()`: Clear all memory objects and points-to results
/// - `clearInterfaceResult()`: Clear function summary data
///
/// **Access Path Queries:**
/// - `getArgLevel(path)`: Compute nesting depth of access path
/// - `getFullAccessPath(val)`: Expand access path to root (arg/global)
/// - `getFullOutputAccessPath(idx)`: Get complete path for output
///
/// **Inter-procedural Mapping:**
/// - `getCallerObj()`: Map callee symbolic object to caller objects
/// - `getCallerEscapeObj()`: Map callee escaped object to caller object
///
/// **Interface Utilities:**
/// - `isPseudoInput()`: Check if value is pseudo-argument (side-effect input)
/// - `isPseudoInterface()`: Check if argument is synthetic
/// - `isSameInterface()`: Compare two function summaries
/// - `isPure()`: Check if function has no side-effects
///
/// **Miscellaneous:**
/// - `getReturnInst()`: Collect all return instructions
/// - `getSequenceNum()`: Get instruction sequence number
/// - `getInlineApDepth()`: Get access path depth limit
///
/// These utilities support the main analysis algorithms and inter-procedural
/// summary generation/application.
///
/// @see IntraProceduralAnalysis.h for class context
/// @see SummaryBuilder.cpp for summary construction using these utilities

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace std;

void IntraLotusAA::getReturnInst() {
  Function *F = analyzed_func;
  assert(F && "NULL base function");

  for (BasicBlock &bb : *F) {
    if (ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
      ret_insts[ret] = getUnitRegion(ret->getParent());
    }
  }
}

int IntraLotusAA::getSequenceNum(Value *val) {
  auto it = value_seq.find(val);
  return (it == value_seq.end()) ? VALUE_SEQ_UNDEF : it->second;
}

int IntraLotusAA::getInlineApDepth() { return inline_ap_depth; }

PTGraph *IntraLotusAA::getPtGraph(Function *F) {
  return lotus_aa->getPtGraph(F);
}

void IntraLotusAA::collectGuardedValueFlowLoadValues(LoadInst *load,
                                                     mem_value_t &result) {
  collectPathSensitiveLoadValues(load, result, false);
}

void IntraLotusAA::collectGuardedValueFlowCallsiteSummaryInputs(
    CallBase *call, std::vector<mem_value_t> &summary_values) {
  summary_values.clear();
  if (!call || IntraLotusAAConfig::lotus_restrict_summary_ap_depth < 1)
    return;

  summary_values.emplace_back();

  std::set<MemObject *, mem_obj_cmp> visited;
  std::map<MemObject *, path_cond_t, mem_obj_cmp> cur_ap_level_object;
  mem_value_t cur_ap_level_value;
  int cur_ap_level = 0;

  for (MemObject *global_object : global_objects) {
    cur_ap_level_object[global_object] = getEmptyCond();
    visited.insert(global_object);
  }

  for (unsigned i = 0; i < call->arg_size(); i++) {
    Value *arg = call->getArgOperand(i);
    PTResult *pts_result = findPTResult(arg, false);
    if (!pts_result)
      continue;

    PTResultIterator result_iter(pts_result, this);
    for (auto &pts : result_iter) {
      MemObject *pt_obj = pts.first->getObj();
      auto it = cur_ap_level_object.find(pt_obj);
      if (it != cur_ap_level_object.end()) {
        it->second = findOrCreateOrRegion(it->second, pts.second);
      } else {
        cur_ap_level_object[pt_obj] = pts.second;
      }
      visited.insert(pt_obj);
    }
  }

  while (true) {
    cur_ap_level++;
    cur_ap_level_value.clear();

    for (auto &obj_item : cur_ap_level_object) {
      MemObject *cur_obj = obj_item.first;
      path_cond_t cur_cond = obj_item.second;
      auto &updated_offsets = cur_obj->getUpdatedOffset();
      for (auto &offset_iter : updated_offsets) {
        int64_t offset = offset_iter.first;
        ObjectLocator *locator = cur_obj->findLocator(offset, false);
        if (!locator)
          continue;

        if (IntraLotusAAConfig::lotus_enable_summary_value) {
          locator->getValues(call, cur_cond, cur_ap_level_value, nullptr,
                             ObjectLocator::FUNC_LEVEL_UNDEFINED, true,
                             func_obj ? func_obj->findLocator(0, false)
                                      : nullptr,
                             true);
        } else {
          locator->getValues(call, cur_cond, cur_ap_level_value);
        }
      }
    }

    refineResult(cur_ap_level_value);
    summary_values.push_back(cur_ap_level_value);

    if (cur_ap_level >= IntraLotusAAConfig::lotus_restrict_summary_ap_depth)
      break;

    cur_ap_level_object.clear();
    for (mem_value_item_t &value_item : cur_ap_level_value) {
      Value *val = value_item.val;
      PTResult *pts_result = findPTResult(val, false);
      if (!pts_result)
        continue;

      PTResultIterator result_iter(pts_result, this);
      for (auto &pts : result_iter) {
        MemObject *pt_obj = pts.first->getObj();
        auto it = cur_ap_level_object.find(pt_obj);
        if (it != cur_ap_level_object.end()) {
          it->second = findOrCreateOrRegion(it->second, pts.second);
        } else if (!visited.count(pt_obj)) {
          cur_ap_level_object[pt_obj] = pts.second;
        }
      }
    }

    if (cur_ap_level_object.empty())
      break;
  }
}

// Clear intermediate results for memory efficiency
void IntraLotusAA::clearIntermediatePtsResult() {
  lotus_clear_hash(&escape_source);
  lotus_clear_hash(&ret_insts);
  lotus_clear_hash(&real_to_pseudo_map);
  lotus_clear_hash(&pseudo_to_real_map);
}

void IntraLotusAA::clearIntermediateCgResult() {
  // Clear temporary CG data
  lotus_clear_hash(&func_arg);
  lotus_clear_hash(&thread_arg);
}

void IntraLotusAA::clearGlobalCgResult() {
  lotus_clear_hash(&cg_resolve_result);
  lotus_clear_hash(&input_cg_summary);
  output_cg_summary.clear();
}

void IntraLotusAA::clearMemObjectResult() {
  lotus_clear_hash(&guarded_points_to_signature_cache_);
  lotus_clear_hash(&must_kill_forests);
  lotus_clear_hash(&reachability_cache);
  lotus_clear_hash(&avoiding_reachability_cache);

  for (auto &it : pt_results) {
    if (it.second != NullPTS)
      delete it.second;
  }

  lotus_clear_hash(&pt_results);

  for (auto &obj : mem_objs) {
    obj.first->clear();
  }

  lotus_clear_hash(&load_category);
  lotus_clear_hash(&value_seq);
}

void IntraLotusAA::clearInterfaceResult() {
  lotus_clear_hash(&inputs);
  lotus_clear_hash(&inputs_func_level);
  lotus_clear_hash(&pseudo_input_indices);
  lotus_clear_hash(&escape_obj_path);
  lotus_clear_hash(&escape_ret_path);
  lotus_clear_hash(&summary_inputs_idx);

  for (OutputItem *item : outputs) {
    delete item;
  }
  outputs.clear();

  for (mem_value_t *vals : summary_outputs) {
    vals->clear();
  }
  for (set<Value *, llvm_cmp> *vals : summary_inputs) {
    vals->clear();
  }
}

// Access path utilities
int IntraLotusAA::getArgLevel(AccessPath &path) {
  int result = 1;
  Value *parent_ptr = path.getParentPtr();
  AccessPath parent_path = path;

  while (parent_ptr && isPseudoInput(parent_ptr)) {
    result++;
    parent_path = inputs[parent_ptr];
    parent_ptr = parent_path.getParentPtr();
  }

  return result;
}

bool IntraLotusAA::isPseudoInput(Value *val) { return inputs.count(val) > 0; }

bool IntraLotusAA::isPseudoInterface(Value *target) {
  if (Argument *arg = dyn_cast<Argument>(target)) {
    if (!arg->getParent())
      return true;
  }
  return false;
}

void IntraLotusAA::onTimeOut() {
  if (timer) {
    timer->suspend();
  }
  is_timeout_found = true;
  is_considered_as_library = true;
  clearIntermediatePtsResult();
  clearIntermediateCgResult();
  clearGlobalCgResult();
  clearInterfaceResult();
}

void IntraLotusAA::setTimer(unsigned duration) {
  if (is_timeout_found)
    return;
  if (timer)
    delete timer;
  timer = new Timer(duration, [this]() { onTimeOut(); }, 5);
}

void IntraLotusAA::getFullAccessPath(
    Value *target_val, std::vector<std::pair<Value *, int64_t>> &result,
    bool *is_from_return) {
  result.clear();
  if (is_from_return)
    *is_from_return = false;

  if (inputs.count(target_val)) {
    AccessPath ap = inputs[target_val];
    getFullAccessPath(ap, result, is_from_return);
  } else {
    result.push_back({target_val, 0});
  }
}

void IntraLotusAA::getFullAccessPath(
    AccessPath &ap, std::vector<std::pair<Value *, int64_t>> &result,
    bool *is_from_return) {
  result.clear();
  if (is_from_return)
    *is_from_return = false;
  AccessPath curr_ap = ap;

  while (true) {
    Value *base_ptr = curr_ap.getParentPtr();
    int64_t offset = curr_ap.getOffset();
    result.push_back({base_ptr, offset});

    if (inputs.count(base_ptr)) {
      curr_ap = inputs[base_ptr];
    } else if (isa<GlobalValue>(base_ptr)) {
      return; // Reached base
    } else if (escape_ret_path.count(base_ptr)) {
      auto &ret_path = escape_ret_path[base_ptr];
      result.push_back({ret_path.first, ret_path.second});
      if (is_from_return)
        *is_from_return = true;
      return;
    } else if (escape_obj_path.count(base_ptr)) {
      auto &obj_path = escape_obj_path[base_ptr];
      curr_ap = obj_path.first;
      result.back().second = obj_path.second;
    } else {
      // Reached local variable or argument
      if (Argument *base_arg = dyn_cast<Argument>(base_ptr)) {
        if (base_arg->getParent()) {
          // Real argument - stop
          return;
        }
      }
      // Local variable - clear result
      result.clear();
      return;
    }
  }
}

void IntraLotusAA::getFullOutputAccessPath(
    int output_index, std::vector<std::pair<Value *, int64_t>> &result,
    bool *is_from_return) {
  result.clear();
  if (is_from_return)
    *is_from_return = false;

  if (output_index <= 0 || (unsigned)output_index >= outputs.size()) {
    return; // Invalid index or common return
  }

  OutputItem *output = outputs[output_index];
  AccessPath ap = output->getSymbolicInfo();
  getFullAccessPath(ap, result, is_from_return);
}

// Get caller object mapping
void IntraLotusAA::getCallerObj(
    Value *call, Function *callee, SymbolicMemObject *calleeObj,
    std::vector<std::pair<MemObject *, int64_t>> &result) {
  Value *calleeArg = calleeObj->getAllocSite();

  if (!func_arg.count(call) || !func_arg[call].count(callee))
    return;

  mem_value_t &arg_result = func_arg[call][callee][calleeArg];

  for (auto &item : arg_result) {
    Value *parent_value = item.val;

    if (parent_value == LocValue::FREE_VARIABLE ||
        parent_value == LocValue::UNDEF_VALUE ||
        parent_value == LocValue::SUMMARY_VALUE)
      continue;

    PTResult *pts = findPTResult(parent_value);
    if (!pts)
      continue;

    PTResultIterator ptr_iter(pts, this);

    for (auto &point_to_item : ptr_iter) {
      ObjectLocator *loc = point_to_item.first;
      MemObject *obj = loc->getObj();
      if (!obj->isValid())
        continue;

      result.push_back({obj, loc->getOffset()});
    }
  }
}

MemObject *IntraLotusAA::getCallerEscapeObj(Value *call, Function *callee,
                                            MemObject *calleeObj) {
  if (!func_escape.count(call) || !func_escape[call].count(callee))
    return nullptr;

  auto &escape = func_escape[call][callee];

  // Handle merged objects
  if (real_to_pseudo_map.count(calleeObj))
    calleeObj = real_to_pseudo_map[calleeObj];

  auto it = escape.find(calleeObj);
  return (it != escape.end()) ? it->second : nullptr;
}

bool IntraLotusAA::isSameInterface(IntraLotusAA *to_compare) {
  if (analyzed_func != to_compare->analyzed_func)
    return false;
  if (inputs.size() != to_compare->inputs.size())
    return false;
  if (outputs.size() != to_compare->outputs.size())
    return false;

  // Compare input access paths
  for (auto &input_pair : inputs) {
    Value *arg = input_pair.first;
    if (!to_compare->inputs.count(arg))
      return false;
    AccessPath &ap1 = input_pair.second;
    AccessPath &ap2 = to_compare->inputs[arg];
    if (ap1.getParentPtr() != ap2.getParentPtr() ||
        ap1.getOffset() != ap2.getOffset())
      return false;
  }

  // Compare output symbolic info (access paths)
  for (size_t i = 0; i < outputs.size(); i++) {
    OutputItem *o1 = outputs[i];
    OutputItem *o2 = to_compare->outputs[i];
    if (o1->getType() != o2->getType())
      return false;
    AccessPath &ap1 = o1->getSymbolicInfo();
    AccessPath &ap2 = o2->getSymbolicInfo();
    if (ap1.getParentPtr() != ap2.getParentPtr() ||
        ap1.getOffset() != ap2.getOffset())
      return false;
  }

  // Compare escaped object count
  if (escape_objs.size() != to_compare->escape_objs.size())
    return false;

  return true;
}


bool IntraLotusAA::isPure() {
  // Pure if no side-effect outputs and no escaped objects
  return outputs.size() <= 1 && escape_objs.empty();
}
