/// @file SummaryBuilder.cpp
/// @brief Function summary construction for context-sensitive inter-procedural
/// analysis
///
/// This file implements the **function summary generation** phase of LotusAA.
/// After intra-procedural pointer analysis completes, these algorithms extract
/// a compact summary describing the function's behavior for use at call sites.
///
/// **Function Summary Components:**
///
/// 1. **Escaped Objects** (`collectEscapedObjects`):
///    - Objects that escape function scope (returned or stored in globals/args)
///    - Computed via reachability from return values and symbolic objects
///    - Includes object merging heuristic for scalability
///
/// 2. **Output Summary** (`collectOutputs`):
///    - Return value (index 0)
///    - Side-effect outputs: modified memory locations (**p, *global, etc.)
///    - For each output: symbolic path + values + points-to information
///
/// 3. **Input Summary** (`collectInputs`):
///    - Side-effect inputs: dereferenced arguments (**p, (*p)->field, etc.)
///    - Generated on-demand when memory reads can't be resolved locally
///    - Represented as pseudo-arguments with access paths
///
/// 4. **Interface Finalization** (`finalizeInterface`):
///    - Prunes deep access paths based on heuristics
///    - Balances precision vs. scalability
///    - Self-adjusting based on interface complexity
///
/// **Summary Application:**
/// Summaries enable **modular analysis** - analyze each function once, apply at
/// all call sites. Critical for scalability on large programs.
///
/// **Example Summary:**
/// ```c
/// int* f(int **p) {
///   *p = malloc(sizeof(int));  // Side-effect output: *p
///   return *p;                 // Return output
/// }
/// // Summary:
/// // - Inputs: {}
/// // - Outputs: {[0] return, [1] *p}
/// // - Escaped: {malloc object}
/// ```
///
/// @see IntraProceduralAnalysis.h for OutputItem and AccessPath data structures
/// @see CallHandling.cpp for summary application at call sites

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"
#include "Alias/LotusAA/Support/Config.h"

#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

void IntraLotusAA::collectEscapedObjects(
    map<MemObject *, MemObject *, mem_obj_cmp> &real_to_pseudo_map,
    map<MemObject *, set<MemObject *, mem_obj_cmp>, mem_obj_cmp>
        &pseudo_to_real_map) {

  // Caches for single-pointed object detection
  map<ObjectLocator *, set<MemObject *, mem_obj_cmp>, obj_loc_cmp>
      single_pointed_objects;
  map<MemObject *, ObjectLocator *, mem_obj_cmp> obj_pointers;
  set<Type *> large_structure_type;

  // Worklist of reachable objects
  std::vector<MemObject *> reachable_worklist;

  // Add symbolic objects (arguments/globals) to worklist
  for (auto &obj_pair : mem_objs) {
    MemObject *obj = obj_pair.first;
    if (obj->getKind() == MemObject::SYMBOLIC) {
      reachable_worklist.push_back(obj);
    }
  }

  // Add objects reachable from return values
  Function *parent_func = analyzed_func;
  if (parent_func->getReturnType()->isPointerTy()) {
    for (auto &ret_pair : ret_insts) {
      ReturnInst *ret = ret_pair.first;
      Value *ret_val = ret->getReturnValue();
      assert(ret_val && "Invalid return value");

      PTResult *pt_result = processBasePointer(ret_val);
      PTResultIterator ptr_iter(pt_result, this);

      for (auto &point_to_item : ptr_iter) {
        ObjectLocator *loc = point_to_item.first;
        MemObject *obj = loc->getObj();
        int64_t offset = loc->getOffset();

        if (escape_objs.count(obj) == 0 &&
            obj->getKind() == MemObject::CONCRETE && !obj->isNull() &&
            !obj->isUnknown()) {

          escape_objs.insert(obj);

          Value *obj_source = obj->getAllocSite();
          if (obj_source) {
            escape_ret_path[obj_source] = {ret_val, offset};
          }

          reachable_worklist.push_back(obj);

          if (offset == 0) {
            single_pointed_objects[nullptr].insert(obj);
            obj_pointers[obj] = nullptr;
          }
        } else if (escape_objs.count(obj) && obj_pointers.count(obj)) {
          // Multiple pointers - remove from single-pointed
          ObjectLocator *curr_loc = obj_pointers[obj];
          obj_pointers.erase(obj);
          single_pointed_objects[curr_loc].erase(obj);
        }
      }
    }
  }

  // Expand reachable set
  while (!reachable_worklist.empty()) {
    MemObject *cur_obj = reachable_worklist.back();
    Value *cur_obj_source = cur_obj->getAllocSite();
    Type *parent_type = cur_obj->guessType();
    reachable_worklist.pop_back();

    for (auto &ptr_offset_pair : cur_obj->getUpdatedOffset()) {
      int64_t ptr_offset = ptr_offset_pair.first;
      Type *child_type = ptr_offset_pair.second;

      if (IntraLotusAAConfig::lotus_restrict_inter_structure != -1 &&
          child_type == parent_type) {
        if (auto *pointer_type = dyn_cast<PointerType>(child_type)) {
          if (pointer_type->getPointerElementType()->isStructTy())
            large_structure_type.insert(child_type);
        }
      }

      mem_value_t res;
      for (auto &ret_pair : ret_insts) {
        ReturnInst *ret = ret_pair.first;
        path_cond_t cond = ret_pair.second;
        ObjectLocator *locator = cur_obj->findLocator(ptr_offset, true);
        locator->getValues(ret, cond, res, nullptr,
                           ObjectLocator::FUNC_LEVEL_UNDEFINED, true,
                           func_obj ? func_obj->findLocator(0, false) : nullptr,
                           true);
      }

      refineResult(res);

      for (auto &item : res) {
        Value *val = item.val;
        PTResult *pt_result = findPTResult(val, false);
        if (!pt_result)
          continue;

        PTResultIterator ptr_iter(pt_result, this);
        for (auto &point_to_item : ptr_iter) {
          ObjectLocator *loc = point_to_item.first;
          MemObject *obj = loc->getObj();
          int64_t offset = loc->getOffset();

          // Handle globals separately
          if (isa_and_nonnull<GlobalValue>(obj->getAllocSite())) {
            if (!escape_objs.count(obj)) {
              escape_objs.insert(obj);

              Value *obj_source = obj->getAllocSite();
              if (obj_source) {
                escape_obj_path[obj_source] = {
                    AccessPath(cur_obj_source, ptr_offset), offset};
              }

              reachable_worklist.push_back(obj);
            }
          } else if (escape_objs.count(obj) == 0 &&
                     obj->getKind() == MemObject::CONCRETE && !obj->isNull() &&
                     !obj->isUnknown()) {

            escape_objs.insert(obj);

            Value *obj_source = obj->getAllocSite();
            if (obj_source) {
              escape_obj_path[obj_source] = {
                  AccessPath(cur_obj_source, ptr_offset), offset};
            }

            reachable_worklist.push_back(obj);

            if (offset == 0) {
              single_pointed_objects[cur_obj->findLocator(ptr_offset, true)]
                  .insert(obj);
              obj_pointers[obj] = cur_obj->findLocator(ptr_offset, true);
            }
          } else if (escape_objs.count(obj) && obj_pointers.count(obj)) {
            ObjectLocator *curr_loc = obj_pointers[obj];
            obj_pointers.erase(obj);
            single_pointed_objects[curr_loc].erase(obj);
          }
        }
      }
    }
  }

  // Erase objects to merge (single-pointed objects with multiple instances)
  std::vector<ObjectLocator *> non_redundant_locators;
  for (auto &loc_objs_pair : single_pointed_objects) {
    ObjectLocator *locator = loc_objs_pair.first;
    set<MemObject *, mem_obj_cmp> &tmp_esc_objs = loc_objs_pair.second;

    if (tmp_esc_objs.size() >= 2) {
      // Redundant objects - merge them
      for (MemObject *tmp_esc_obj : tmp_esc_objs) {
        escape_objs.erase(tmp_esc_obj);
      }
    } else {
      non_redundant_locators.push_back(locator);
      for (MemObject *tmp_esc_obj : tmp_esc_objs) {
        obj_pointers.erase(tmp_esc_obj);
      }
    }
  }

  for (ObjectLocator *non_redundant : non_redundant_locators) {
    single_pointed_objects.erase(non_redundant);
  }

  // Create pseudo objects for merged objects
  for (auto &loc_objs_pair : single_pointed_objects) {
    set<MemObject *, mem_obj_cmp> &objs_to_merge = loc_objs_pair.second;

    auto first_obj = objs_to_merge.begin();
    Value *alloca_site = (*first_obj)->getAllocSite();

    MemObject *pseudo_obj = newObject(alloca_site, MemObject::CONCRETE);

    for (MemObject *obj : objs_to_merge) {
      pseudo_to_real_map[pseudo_obj].insert(obj);
      real_to_pseudo_map[obj] = pseudo_obj;
    }

    escape_objs.insert(pseudo_obj);
  }

  if (IntraLotusAAConfig::lotus_restrict_inter_structure != -1) {
    map<Type *, MemObject *> pseudo_objs_for_type;
    std::vector<MemObject *> merged;

    for (MemObject *esc_obj : escape_objs) {
      Type *esc_obj_type = esc_obj->guessType();
      if (large_structure_type.count(esc_obj_type) == 0)
        continue;

      merged.push_back(esc_obj);
      MemObject *pseudo_obj = nullptr;
      auto it = pseudo_objs_for_type.find(esc_obj_type);
      if (it != pseudo_objs_for_type.end()) {
        pseudo_obj = it->second;
      } else {
        pseudo_obj = newObject(esc_obj->getAllocSite(), MemObject::CONCRETE);
        pseudo_objs_for_type[esc_obj_type] = pseudo_obj;
      }

      pseudo_to_real_map[pseudo_obj].insert(esc_obj);
      real_to_pseudo_map[esc_obj] = pseudo_obj;
    }

    for (MemObject *obj : merged) {
      escape_objs.erase(obj);
    }

    for (auto &type_pair : pseudo_objs_for_type) {
      escape_objs.insert(type_pair.second);
    }
  }

  // Record escape sources
  for (MemObject *obj : escape_objs) {
    escape_source.insert(obj->getAllocSite());
  }
}

void IntraLotusAA::collectOutputs() {
  // Create return value output (index 0)
  OutputItem *ret_item = new OutputItem;
  Function *parent_func = analyzed_func;
  ret_item->setType(parent_func->getReturnType());

  if (!ret_insts.empty() && !parent_func->getReturnType()->isVoidTy()) {
    for (auto &ret_pair : ret_insts) {
      ReturnInst *ret = ret_pair.first;
      Value *ret_value = ret->getReturnValue();
      path_cond_t cond = ret_pair.second;
      ret_item->getVal()[ret].push_back(
          mem_value_item_t(cond, nullptr, ret_value, 1.0f));
    }
    ret_item->symbolic_info.reset(nullptr, 0);
    ret_item->func_level = 0;
  }
  outputs.push_back(ret_item);

  if (ret_insts.empty())
    return; // Function doesn't return

  // Collect escaped objects
  collectEscapedObjects(real_to_pseudo_map, pseudo_to_real_map);

  // Collect side-effect outputs from merged objects
  for (auto &pseudo_pair : pseudo_to_real_map) {
    MemObject *pseudo_obj = pseudo_pair.first;
    set<MemObject *, mem_obj_cmp> &objs_to_merge = pseudo_pair.second;
    map<int64_t, OutputItem *> updated_offsets_n_results;
    Value *alloca_site = pseudo_obj->getAllocSite();

    for (MemObject *merge_obj : objs_to_merge) {
      map<int64_t, Type *> &updated_offset = merge_obj->getUpdatedOffset();

      for (auto &offset_item : updated_offset) {
        int64_t offset = offset_item.first;
        Type *type = offset_item.second;
        Type *normalized_type = normalizeType(type);
        ObjectLocator *locator = merge_obj->findLocator(offset, true);
        int func_level = locator->getStoreFunctionLevel();

        if (updated_offsets_n_results.count(offset)) {
          OutputItem *output_item = updated_offsets_n_results[offset];
          if (output_item->func_level == ObjectLocator::FUNC_LEVEL_UNDEFINED ||
              output_item->func_level > func_level) {
            if (func_level != ObjectLocator::FUNC_LEVEL_UNDEFINED)
              output_item->func_level = func_level;
          }

          for (auto &ret_pair : ret_insts) {
            ReturnInst *ret = ret_pair.first;
            path_cond_t cond = ret_pair.second;
            mem_value_t &res = output_item->getVal()[ret];
            locator->getValues(ret, cond, res, normalized_type,
                               ObjectLocator::FUNC_LEVEL_UNDEFINED, true,
                               func_obj ? func_obj->findLocator(0, false)
                                        : nullptr,
                               true);
            refineResult(res);
          }
        } else {
          OutputItem *output_item = new OutputItem;
          updated_offsets_n_results[offset] = output_item;

          for (auto &ret_pair : ret_insts) {
            ReturnInst *ret = ret_pair.first;
            path_cond_t cond = ret_pair.second;
            mem_value_t &res = output_item->getVal()[ret];
            locator->getValues(ret, cond, res, normalized_type,
                               ObjectLocator::FUNC_LEVEL_UNDEFINED, true,
                               func_obj ? func_obj->findLocator(0, false)
                                        : nullptr,
                               true);
          }

          AccessPath &info = output_item->getSymbolicInfo();
          info.reset(alloca_site, offset);
          output_item->setType(type);
          output_item->func_level = func_level;
          outputs.push_back(output_item);
        }
      }
    }
  }

  // Collect updated fields from symbolic/escaped objects
  for (auto &obj_pair : mem_objs) {
    MemObject *obj = obj_pair.first;

    if ((obj->getKind() == MemObject::SYMBOLIC || escape_objs.count(obj)) &&
        obj->getAllocSite() && !obj->getAllocSite()->getType()->isVoidTy()) {

      map<int64_t, Type *> &updated_offset = obj->getUpdatedOffset();

      for (auto &offset_item : updated_offset) {
        int64_t offset = offset_item.first;
        Type *type = offset_item.second;
        Type *normalized_type = normalizeType(type);

        OutputItem *output_item = new OutputItem;
        ObjectLocator *locator = obj->findLocator(offset, true);
        int func_level = locator->getStoreFunctionLevel();

        for (auto &ret_pair : ret_insts) {
          ReturnInst *ret = ret_pair.first;
          path_cond_t cond = ret_pair.second;
          mem_value_t &res = output_item->getVal()[ret];
          locator->getValues(ret, cond, res, normalized_type,
                             ObjectLocator::FUNC_LEVEL_UNDEFINED, true,
                             func_obj ? func_obj->findLocator(0, false)
                                      : nullptr,
                             true);
          refineResult(res);
        }

        AccessPath &info = output_item->getSymbolicInfo();
        info.reset(obj->getAllocSite(), offset);
        output_item->setType(type);
        output_item->func_level = func_level;
        outputs.push_back(output_item);
      }
    }
  }

  auto refine_output_value = [this](std::vector<OutputItem *> &output_items) {
    std::map<std::pair<Value *, Instruction *>, std::pair<path_cond_t, float>>
        merged_values;

    for (auto *out : output_items) {
      for (auto &ret_mem_pair : out->getVal()) {
        mem_value_t &mem_vals = ret_mem_pair.second;
        for (auto &mem : mem_vals) {
          auto key = std::make_pair(mem.val, mem.pos);
          auto it = merged_values.find(key);
          if (it == merged_values.end()) {
            merged_values.emplace(key, std::make_pair(mem.cond, mem.confidence));
          } else {
            it->second.first =
                findOrCreateOrRegion(it->second.first, mem.cond);
            it->second.second = mem_value_item_t::compute_or_confidence(
                it->second.second, mem.confidence);
          }
        }
      }
    }

    for (auto *out : output_items) {
      for (auto &ret_mem_pair : out->getVal()) {
        mem_value_t &mem_vals = ret_mem_pair.second;
        mem_value_t refined_mem_vals;
        refined_mem_vals.reserve(mem_vals.size());
        for (auto &mem : mem_vals) {
          auto merged = merged_values[std::make_pair(mem.val, mem.pos)];
          refined_mem_vals.emplace_back(merged.first, mem.pos, mem.val,
                                        merged.second);
        }
        mem_vals = std::move(refined_mem_vals);
      }
    }
  };

  refine_output_value(outputs);

  // Record point-to changes for outputs
  for (auto &output_item : outputs) {
    map<ReturnInst *, mem_value_t, llvm_cmp> &mem_value_map =
        output_item->getVal();

    for (auto &mem_value_element : mem_value_map) {
      mem_value_t &mem_value = mem_value_element.second;

      for (mem_value_item_t &value_item : mem_value) {
        Value *val = value_item.val;

        if (val != LocValue::FREE_VARIABLE) {
          PTResult *pt_result = findPTResult(val, false);
          if (pt_result) {
            PTResultIterator iter(pt_result, this);

            for (auto &point_to_item : iter) {
              ObjectLocator *loc = point_to_item.first;
              int64_t offset = loc->getOffset();
              MemObject *point_to_obj = loc->getObj();

              // Handle merged escaped objects
              if (real_to_pseudo_map.count(point_to_obj)) {
                MemObject *pseudo_obj = real_to_pseudo_map[point_to_obj];
                loc = pseudo_obj->findLocator(offset, true);
              }

              Value *parent_val = loc->getObj()->getAllocSite();
              AccessPath output_info(parent_val, offset);
              path_cond_t final_cond =
                  findOrCreateAndRegion(value_item.cond, point_to_item.second);
              output_item->pseudo_pts.push_back(
                  std::make_pair(final_cond, output_info));
            }
          }
        }
      }
    }
  }
}

void IntraLotusAA::collectInputs() {
  // Collect pseudo-arguments from symbolic objects
  for (auto &obj_pair : mem_objs) {
    MemObject *obj = obj_pair.first;

    if (IntraLotusAAConfig::lotus_restrict_inter_structure != -1 &&
        real_to_pseudo_map.count(obj) != 0) {
      continue;
    }

    if (obj->getKind() == MemObject::SYMBOLIC) {
      SymbolicMemObject *sobj = cast<SymbolicMemObject>(obj);

      for (auto &pseudo_pair : sobj->getPseudoArgs()) {
        ObjectLocator *locator = pseudo_pair.first;
        Argument *arg = pseudo_pair.second;

        AccessPath info(locator->getObj()->getAllocSite(),
                        locator->getOffset());
        assert(!isPseudoInput(arg) && "Multiple uses for same pseudo_arg");

        inputs[arg] = info;
        inputs_func_level[arg] = locator->getLoadFunctionLevel();
      }
    }
  }

  pseudo_input_indices.clear();
  unsigned next_index = 0;
  for (const auto &input_item : inputs)
    pseudo_input_indices[input_item.first] = next_index++;

  // Verify completeness (if testing enabled)
  if (IntraLotusAAConfig::lotus_test_correctness) {
    for (size_t i = 0; i < outputs.size(); i++) {
      OutputItem *output = outputs[i];
      Value *parent_ptr = output->getSymbolicInfo().getParentPtr();

      if (!parent_ptr)
        continue;
      if (isa<GlobalValue>(parent_ptr))
        continue;

      bool found = false;

      // Check if in escaped objects
      for (auto *obj : escape_objs) {
        if (obj->getAllocSite() == parent_ptr) {
          found = true;
          break;
        }
      }

      if (!found) {
        // Check if real argument
        for (Argument &arg : analyzed_func->args()) {
          if (parent_ptr == &arg) {
            found = true;
            break;
          }
        }
      }

      assert((found || isPseudoInput(parent_ptr)) &&
             "Output parent not in inputs or escape set");
    }
  }
}

void IntraLotusAA::finalizeInterface() {
  // Determine effective AP level restriction
  int lotus_restrict_ap_level_adjust = 0;

  if (IntraLotusAAConfig::lotus_restrict_inline_size < 0) {
    lotus_restrict_ap_level_adjust =
        IntraLotusAAConfig::lotus_restrict_ap_level;
  } else if ((int)escape_objs.size() >=
             IntraLotusAAConfig::lotus_restrict_inline_size) {
    lotus_restrict_ap_level_adjust = 0;
  } else {
    // Keep the exact self-adjusting interface pruning heuristic.
    const int CACHE_SIZE = 10;
    int cache_input[CACHE_SIZE] = {0};
    int cache_output[CACHE_SIZE] = {0};
    int input_sum = 0;
    int output_sum = 0;
    int next_cache_start = 0;

    for (lotus_restrict_ap_level_adjust = 0;
         lotus_restrict_ap_level_adjust <=
         IntraLotusAAConfig::lotus_restrict_ap_level;
         lotus_restrict_ap_level_adjust++) {

      if (lotus_restrict_ap_level_adjust == next_cache_start) {
        // Build cache for next batch
        for (int idx = 0; idx < CACHE_SIZE; idx++) {
          cache_output[idx] = 0;
          cache_input[idx] = 0;
        }

        for (size_t idx = 1; idx < outputs.size(); idx++) {
          OutputItem *output_item = outputs[idx];
          AccessPath &path = output_item->getSymbolicInfo();
          int level = getArgLevel(path);
          if (level >= next_cache_start &&
              level < next_cache_start + CACHE_SIZE) {
            cache_output[level - next_cache_start]++;
          }
        }

        for (auto &input_item : inputs) {
          AccessPath &path = input_item.second;
          int level = getArgLevel(path);
          if (level >= next_cache_start &&
              level < next_cache_start + CACHE_SIZE) {
            cache_input[level - next_cache_start]++;
          }
        }

        next_cache_start += CACHE_SIZE;
      }

      int cache_idx =
          lotus_restrict_ap_level_adjust - (next_cache_start - CACHE_SIZE);
      input_sum += cache_input[cache_idx];
      output_sum += cache_output[cache_idx];

      if (input_sum >= IntraLotusAAConfig::lotus_restrict_inline_size ||
          output_sum >= IntraLotusAAConfig::lotus_restrict_inline_size) {
        break;
      }

      if (cache_input[cache_idx] == 0 && cache_output[cache_idx] == 0 &&
          lotus_restrict_ap_level_adjust != 0) {
        lotus_restrict_ap_level_adjust =
            LotusConfig::MAXIMAL_SUMMARY_AP_DEPTH + 1;
        break;
      }
    }
  }

  lotus_restrict_ap_level_adjust--;

  // Filter outputs by AP level and function level
  std::vector<OutputItem *> new_outputs;
  new_outputs.push_back(outputs[0]); // Always keep return value

  for (size_t idx = 1; idx < outputs.size(); idx++) {
    OutputItem *output_item = outputs[idx];
    AccessPath &path = output_item->getSymbolicInfo();
    int level = getArgLevel(path);
    int func_level = output_item->getFuncLevel();

    if ((level <= lotus_restrict_ap_level_adjust ||
         lotus_restrict_ap_level_adjust < 0) &&
        (func_level < IntraLotusAAConfig::lotus_restrict_inline_depth ||
         IntraLotusAAConfig::lotus_restrict_inline_depth < 0)) {
      new_outputs.push_back(output_item);
    } else {
      for (auto &ret_val_pair : output_item->getVal()) {
        mem_value_t *vals = &ret_val_pair.second;
        mem_value_t *summary_vals;
        if (level > IntraLotusAAConfig::lotus_restrict_summary_ap_depth) {
          summary_vals = summary_outputs[0];
        } else {
          summary_vals = summary_outputs[level];
        }
        for (mem_value_item_t &mem_item : *vals)
          summary_vals->push_back(mem_item);
      }
      delete output_item;
    }
  }

  outputs.clear();
  outputs = std::move(new_outputs);

  for (mem_value_t *vals : summary_outputs) {
    refineResult(*vals);
  }

  // Filter inputs
  std::vector<Value *> to_remove;
  for (auto &input_item : inputs) {
    AccessPath &path = input_item.second;
    Value *arg = input_item.first;
    int level = getArgLevel(path);
    int func_level = inputs_func_level[arg];

    if (!((level <= lotus_restrict_ap_level_adjust ||
           lotus_restrict_ap_level_adjust < 0) &&
          (func_level < IntraLotusAAConfig::lotus_restrict_inline_depth ||
           IntraLotusAAConfig::lotus_restrict_inline_depth < 0))) {
      to_remove.push_back(arg);
      int summary_idx = 0;
      if (level > IntraLotusAAConfig::lotus_restrict_summary_ap_depth) {
        summary_idx = 0;
      } else {
        summary_idx = level;
      }
      summary_inputs[summary_idx]->insert(arg);
      summary_inputs_idx[arg] = summary_idx;
    }
  }

  for (Value *arg : to_remove) {
    inputs.erase(arg);
  }

  pseudo_input_indices.clear();
  unsigned next_pseudo_input_index = 0;
  for (const auto &input_item : inputs)
    pseudo_input_indices[input_item.first] = next_pseudo_input_index++;

  // Finalize point-to info for outputs
  for (OutputItem *output_item : outputs) {
    auto &point_to = output_item->getPseudoPointTo();

    if (IntraLotusAAConfig::lotus_restrict_output_pts != -1 &&
        static_cast<int>(point_to.size()) >
            IntraLotusAAConfig::lotus_restrict_output_pts) {
      point_to.clear();
      continue;
    }

    map<Value *, map<int64_t, pair<path_cond_t, AccessPath>, less<int64_t>>,
        llvm_cmp>
        should_exist;

    for (auto &point_to_item : point_to) {
      AccessPath &ap = point_to_item.second;
      Value *parent_val = ap.getParentPtr();
      int64_t offset = ap.getOffset();

      if (parent_val == nullptr || isa<GlobalValue>(parent_val) ||
          inputs.count(parent_val) || escape_source.count(parent_val) ||
          (isa<Argument>(parent_val) &&
           cast<Argument>(parent_val)->getParent() != nullptr)) {
        if (should_exist.count(parent_val) &&
            should_exist[parent_val].count(offset)) {
          path_cond_t prev_cond = should_exist[parent_val][offset].first;
          should_exist[parent_val][offset] = std::make_pair(
              findOrCreateOrRegion(prev_cond, point_to_item.first), ap);
        } else {
          should_exist[parent_val][offset] = point_to_item;
        }
      }
    }

    point_to.clear();
    for (auto &value_offsets : should_exist) {
      for (auto &offset_item : value_offsets.second) {
        point_to.push_back(offset_item.second);
      }
    }
  }

  // Record inline depth
  inline_ap_depth = lotus_restrict_ap_level_adjust;
}
