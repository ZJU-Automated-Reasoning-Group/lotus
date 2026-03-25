/// @file CallHandling.cpp
/// @brief Inter-procedural call transfer functions and function summary
/// application
///
/// This file implements the **most complex transfer function** in LotusAA:
/// processing function calls. It handles context-sensitive inter-procedural
/// analysis by applying callee summaries to call sites.
///
/// **Core Responsibilities:**
/// 1. **Summary Application**: Apply callee function summaries to call sites
/// 2. **Input Binding**: Map actual arguments to formal parameters
/// 3. **Output Linking**: Connect return values and side-effects to caller
/// context
/// 4. **Escaped Object Handling**: Track objects that escape via function calls
/// 5. **Pseudo-Node Creation**: Create synthetic nodes for inter-procedural
/// values
///
/// **Inter-procedural Analysis Flow:**
/// ```
/// processCall(call_site):
///   for each possible callee:
///     // 1. Bind inputs: actual args → formal params
///     processCalleeInput(callee.inputs, actual_args)
///
///     // 2. Create pseudo-nodes for outputs
///     createPseudoOutputNodes(callee.outputs, call_site)
///
///     // 3. Create escaped objects
///     createEscapedObjects(callee.escape_objs, call_site)
///
///     // 4. Link outputs to caller
///     processCalleeOutput(callee.outputs, callee.escape_objs, call_site)
/// ```
///
/// **Function Summaries:**
/// - **Inputs**: Symbolic arguments + side-effect inputs (e.g., **p for
/// argument p)
/// - **Outputs**: Return value + side-effect outputs (modified memory)
/// - **Escaped Objects**: Stack objects that escape to caller's heap/stack
///
/// **Key Design Decisions:**
/// - Context-sensitive: Different call sites get different pseudo-nodes
/// - Allocation-site sensitive: Distinguish objects by allocation context
/// - Side-effect tracking: Model memory modifications via
/// pseudo-arguments/returns
///
/// **Example:**
/// ```c
/// void f(int **p) { *p = malloc(...); }  // Side-effect output: **p
/// int *q;
/// f(&q);  // After call: q points to allocated object
/// ```
///
/// @see IntraProceduralAnalysis.h for summary data structures
/// @see SummaryBuilder.cpp for summary construction
/// @see createPseudoOutputNodes() for synthetic value creation

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <unordered_map>

#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

static bool isCallsiteFunctionCompatible(CallBase *call, Function *callee) {
  return isPointerAnalysisCallsiteCompatible(call, callee);
}

/// Conservatively handles unknown library calls by invalidating pointer
/// arguments.
///
/// @param call The unknown library call
/// @note Treats all pointer arguments as potentially modified (weak update with
/// NO_VALUE)
void IntraLotusAA::processUnknownLibraryCall(CallBase *call) {
  if (IntraLotusAAConfig::lotus_disable_library_heuristic)
    return;

  if (Function *func = call->getCalledFunction()) {
    if (lotus_aa->getSpecManager().isNoEffect(func))
      return;
  }

  // Mark all pointer arguments as potentially modified
  // TODO: this operation may lead to imprecision in the analysis;
  // Another choicse is to treat UnknownLibraryCall a "noop" (does nothing?)
  for (unsigned i = 0; i < call->arg_size(); i++) {
    Value *arg = call->getArgOperand(i);
    PointerType *pointer_arg_type = dyn_cast<PointerType>(arg->getType());
    if (!pointer_arg_type)
      continue;

    if (i == 0 &&
        pointer_arg_type->getPointerElementType()->isAggregateType()) {
      // Preserve the library heuristic for likely <this> receivers.
      continue;
    }

    processBasePointer(arg);

    PTResult *pt_result = findPTResult(arg, false);
    if (!pt_result)
      continue;

    PTResultIterator iter(pt_result, this);
    for (auto &pt_item : iter) {
      pt_item.first->storeValue(LocValue::NO_VALUE, call, pt_item.second, 0);
    }
  }
}

/// Main transfer function for call instructions - applies callee summaries.
///
/// Processes both direct and indirect calls by applying function summaries.
/// For each possible callee, binds inputs, creates output pseudo-nodes, and
/// links escaped objects.
///
/// @param call The call or invoke instruction
/// @see processCalleeInput(), processCalleeOutput() for detailed steps
void IntraLotusAA::processCall(CallBase *call) {
  if (IntraLotusAAConfig::lotus_restrict_inline_depth == 0) {
    if (call->getType()->isPointerTy()) {
      addPointsTo(call, newObject(call, MemObject::CONCRETE), 0,
                  getEmptyCond());
    }
    return;
  }

  Function *base_func = call->getParent()->getParent();
  auto *callees = lotus_aa->getCallees(base_func, call);

  if (!callees) {
    processUnknownLibraryCall(call);
    return;
  }

  // Process each possible callee
  int callee_idx = -1;
  for (auto &callee_item : *callees) {
    callee_idx++;
    Function *callee = callee_item.first;
    path_cond_t callee_cond = callee_item.second;
    if (callee_idx >= IntraLotusAAConfig::lotus_restrict_cg_size)
      break;

    if (!callee || !isCallsiteFunctionCompatible(call, callee)) {
      continue;
    }

    if (callee_idx == 0) {
      callee_cond = getEmptyCond();
    } else {
      callee_cond = findOrCreateAndRegion(
          callee_cond, getCallTargetCond(call->getCalledOperand(), callee));
    }

    if (!callee || lotus_aa->isBackEdge(base_func, callee)) {
      if (call->getType()->isPointerTy() &&
          (unsigned)callee_idx == callees->size() - 1 &&
          !pt_results.count(call)) {
        addPointsTo(call, newObject(call, MemObject::CONCRETE), 0, callee_cond);
      }
      continue;
    }

    if (lotus_aa->getSpecManager().isAllocator(callee) && callee->empty()) {
      if (call->getType()->isPointerTy() &&
          (unsigned)callee_idx == callees->size() - 1 &&
          !pt_results.count(call)) {
        addPointsTo(call, newObject(call, MemObject::CONCRETE), 0, callee_cond);
      }
      continue;
    }

    auto return_alias = lotus_aa->getSpecManager().getReturnAliasInfo(callee);
    if (!call->getType()->isPointerTy() && !return_alias.empty()) {
      if (!func_new) {
        func_new = new Argument(PTGraph::DEFAULT_POINTER_TYPE);
      }
      addPointsTo(func_new, newObject(call, MemObject::CONCRETE), 0,
                  callee_cond);
    }

    IntraLotusAA *callee_result = lotus_aa->getPtGraph(callee);

    if (!callee_result || callee_result->is_considered_as_library) {
      if (call->getType()->isPointerTy() &&
          static_cast<unsigned>(callee_idx) == callees->size() - 1 &&
          !pt_results.count(call)) {
        addPointsTo(call, newObject(call, MemObject::CONCRETE), 0, callee_cond);
      }

      processUnknownLibraryCall(call);

      if (!IntraLotusAAConfig::lotus_disable_thread_heuristic &&
          callee->getName() == "pthread_create" && call->arg_size() == 4 &&
          isa<Function>(call->getArgOperand(2)) &&
          call->getArgOperand(3)->getType()->isPointerTy()) {
        Function *thread_func = dyn_cast<Function>(call->getArgOperand(2));
        IntraLotusAA *thread_pta = lotus_aa->getPtGraph(thread_func);
        if (thread_pta && !thread_pta->is_considered_as_library) {
          Value *arg = call->getArgOperand(3);
          func_arg_t &arg_result = thread_arg[call];
          vector<Value *> real_args, formal_args;
          real_args.push_back(arg);
          for (Argument &arg_val : thread_func->args()) {
            formal_args.push_back(&arg_val);
          }
          processCalleeInput(thread_pta->getInputs(),
                             thread_pta->inputs_func_level, real_args,
                             formal_args, call, arg_result, callee_cond);
        }
      }

      continue;
    }

    // Process callee summary: inputs, outputs, and escaped objects
    auto &callee_inputs = callee_result->getInputs();
    auto &callee_outputs = callee_result->getOutputs();
    auto &callee_escape = callee_result->getEscapeObjs();

    if (callee_inputs.empty() && callee_outputs.size() == 1 &&
        callee_result->getInlineApDepth() == 0) {
      processUnknownLibraryCall(call);
    }

    func_arg_t &arg_result = func_arg[call][callee];

    std::vector<Value *> formal_args, real_args;
    for (auto &arg : callee->args()) {
      formal_args.push_back(&arg);
    }
    for (unsigned i = 0; i < call->arg_size(); i++) {
      real_args.push_back(call->getArgOperand(i));
    }

    processCalleeInput(callee_inputs, callee_result->inputs_func_level,
                       real_args, formal_args, call, arg_result, callee_cond);
    processCalleeOutput(callee_outputs, callee_escape, call, callee,
                        callee_cond);
  }
}

/// Binds actual arguments to callee's formal parameters and side-effect inputs.
///
/// Maps caller values to callee's symbolic interface, handling both:
/// - Direct arguments (formal parameters)
/// - Side-effect inputs (dereferenced arguments like **p)
///
/// @param callee_input Callee's input summary (formal args + side-effects)
/// @param callee_input_func_level Function nesting level for each input
/// @param real_args Actual argument values at call site
/// @param formal_args Formal parameter values in callee
/// @param callsite The call instruction
/// @param result Output map binding callee inputs to caller values
void IntraLotusAA::processCalleeInput(
    map<Value *, AccessPath, llvm_cmp> &callee_input,
    map<Value *, int, llvm_cmp> &callee_input_func_level,
    std::vector<Value *> &real_args, std::vector<Value *> &formal_args,
    CallBase *callsite, func_arg_t &result, path_cond_t pre_cond) {

  if (!pre_cond)
    pre_cond = getEmptyCond();

  // (1) Collect the real arguments and link the values to pseudo-arguments
  size_t real_size = real_args.size();
  size_t formal_size = formal_args.size();
  for (size_t idx = 0; idx < real_size && idx < formal_size; idx++) {
    Value *formal_arg = formal_args[idx];
    Value *real_arg = real_args[idx];

    mem_value_item_t mem_val_item(pre_cond, nullptr, real_arg, 1.0f);
    result[formal_arg].push_back(mem_val_item);

    if (real_arg->getType()->isPointerTy()) {
      processBasePointer(real_arg);
    }
  }

  // (2) Process the side-effect inputs
  // For each input ptr->idx1->idx2->idx3
  // We first check if it is already processed
  // If it is not processed, we use the value of ptr->idx1->idx2 and compute the
  // value of ptr->idx1->idx2->idx3 If ptr->idx1->idx2 also does not exist, we
  // first use ptr->idx1 to compute ptr->idx1->idx2 We keep doing these steps
  // until the required value is computed or is a global or argument
  set<Value *, llvm_cmp> processed;
  for (auto &iter : callee_input) {
    Value *pseudo_arg = iter.first;
    if (processed.count(pseudo_arg)) {
      continue;
    }

    std::vector<Value *> parents;
    Value *parent_iter = pseudo_arg;
    while ((!processed.count(parent_iter)) &&
           (callee_input.count(parent_iter))) {
      parents.push_back(parent_iter);
      AccessPath &parent_info = callee_input[parent_iter];
      Value *parent_arg = parent_info.getParentPtr();
      parent_iter = parent_arg;
    }

    for (int i = (int)parents.size() - 1; i >= 0; i--) {

      Value *curr_arg_val = parents[i];
      processed.insert(curr_arg_val);
      assert(callee_input.count(curr_arg_val) && "Invalid Value found");
      AccessPath &arg_info = callee_input[curr_arg_val];

      Value *parent_arg = arg_info.getParentPtr();
      int64_t offset = arg_info.getOffset();

      mem_value_t &parent_arg_values = result[parent_arg];

      if (!isPseudoInput(parent_arg)) {
        // The parent arg is a real Argument or a Global Value
        processBasePointer(parent_arg);
        if (isa<GlobalValue>(parent_arg)) {
          // Process the global values on demand
          mem_value_item_t mem_value_item(pre_cond, nullptr, parent_arg, 1.0f);
          parent_arg_values.push_back(mem_value_item);
        } else if (isa<Argument>(parent_arg)) {
          // Arguments are processed before
        } else {
          // Default
        }
      }

      refineResult(parent_arg_values);

      mem_value_t &arg_values = result[curr_arg_val];
      for (auto &parent_value_pair : parent_arg_values) {
        Value *parent_value = parent_value_pair.val;
        if (parent_value == LocValue::FREE_VARIABLE ||
            parent_value == LocValue::UNDEF_VALUE ||
            parent_value == LocValue::SUMMARY_VALUE) {
          continue;
        }

        mem_value_t tmp_values;

        if (findPTResult(parent_value) == nullptr) {
          if (isa<Argument>(parent_value)) {
            // Only when the parent value is an argument (Real Argument/ Side
            // effect input/ Output from callee), we create a new object
            Argument *parent_value_to_arg = dyn_cast<Argument>(parent_value);
            processArg(parent_value_to_arg);
          } else {
            continue;
          }
        }
        loadPtrAt(parent_value, callsite, tmp_values, true, offset,
                  callee_input_func_level[curr_arg_val] + 1,
                  func_obj ? func_obj->findLocator(0, false) : nullptr, true);

        for (auto &tmp_val : tmp_values) {
          path_cond_t new_cond =
              findOrCreateAndRegion(tmp_val.cond, parent_value_pair.cond);
          path_cond_t final_cond = findOrCreateAndRegion(new_cond, pre_cond);
          mem_value_item_t mem_value_item(final_cond, tmp_val.pos, tmp_val.val,
                                          tmp_val.confidence);
          arg_values.push_back(mem_value_item);
        }
      }
      refineResult(arg_values);
    }
  }
}

std::vector<Value *> &
IntraLotusAA::createPseudoOutputNodes(std::vector<OutputItem *> &callee_output,
                                      Instruction *callsite, Function *callee) {

  assert(!func_ret[callsite].count(callee) && "callsite already processed!!!");

  std::vector<Value *> &out_values = func_ret[callsite][callee];
  out_values.push_back(callsite);

  for (size_t idx = 1; idx < callee_output.size(); idx++) {
    OutputItem *output = callee_output[idx];
    Type *output_type = output->getType();

    // LLVM doesn't allow naming void-typed values, so use empty name for void
    // types
    string name_str;
    if (!output_type->isVoidTy()) {
      raw_string_ostream ss(name_str);
      ss << "LPseudoCallSiteOutput_" << callsite << "_" << callee << "_#"
         << idx;
      ss.flush();
    }

    Argument *new_arg = new Argument(output_type, name_str);
    out_values.push_back(new_arg);
    func_pseudo_ret_cache[new_arg] = make_pair(callsite, idx);
  }

  assert(out_values.size() == callee_output.size() &&
         "Incorrect collection of outputs");

  return out_values;
}

void IntraLotusAA::createEscapedObjects(
    set<MemObject *, mem_obj_cmp> &callee_escape, Instruction *callsite,
    Function *callee, map<Value *, MemObject *, llvm_cmp> &escape_object_map) {

  int escape_obj_idx = 0;

  for (MemObject *callee_escape_obj : callee_escape) {
    if (callee_escape_obj == nullptr) {
      continue;
    }

    Value *alloca_site = callee_escape_obj->getAllocSite();
    if (alloca_site == nullptr) {
      // Null Objects and Unknown Objects are not processed
      continue;
    }
    Type *obj_ptr_type = alloca_site->getType();

    // LLVM doesn't allow naming void-typed values, so use empty name for void
    // types
    string name_str;
    if (!obj_ptr_type->isVoidTy()) {
      raw_string_ostream ss(name_str);
      ss << "LCallSiteEscapedObject_" << callsite << "_#" << escape_obj_idx++;
      ss.flush();
    }

    Argument *new_arg = new Argument(obj_ptr_type, name_str);
    func_pseudo_ret_cache[new_arg] = make_pair(callsite, PTR_TO_ESC_OBJ);
    MemObject::ObjKind obj_kind = MemObject::CONCRETE;
    MemObject *escaped_obj_to = newObject(new_arg, obj_kind);
    addPointsTo(new_arg, escaped_obj_to, 0, getEmptyCond());
    escape_object_map[alloca_site] = escaped_obj_to;

    // Cache the escape mapping
    func_escape[callsite][callee][callee_escape_obj] = escaped_obj_to;
  }
}

void IntraLotusAA::linkOutputPointsToResults(
    OutputItem *output, Value *curr_output,
    map<Value *, MemObject *, llvm_cmp> &escape_object_map,
    func_arg_t &callee_func_arg, Instruction *callsite, Function *callee,
    std::set<PTResult *> &visited) {

  auto &callee_point_to = output->getPseudoPointTo();
  PTResult *curr_output_pts = nullptr;
  int func_level = output->getFuncLevel();

  if (func_level == ObjectLocator::FUNC_LEVEL_UNDEFINED) {
    func_level = 0;
    output->func_level = 0;
  }

  // Link the pointer-result and the values
  for (auto &callee_point_to_item : callee_point_to) {
    path_cond_t interface_cond =
        importPathCond(callee_point_to_item.first, callsite, callee);
    AccessPath callee_point_to_item_info = callee_point_to_item.second;
    Value *callee_point_to_item_parent_ptr =
        callee_point_to_item_info.getParentPtr();
    int64_t callee_point_to_item_offset = callee_point_to_item_info.getOffset();

    if (callee_point_to_item_parent_ptr == nullptr) {
      // Pointer pointing to null or unknown object
      curr_output_pts =
          curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
      curr_output_pts->add_target(interface_cond, MemObject::UnknownObj,
                                  callee_point_to_item_offset);
    } else if (isa<GlobalValue>(callee_point_to_item_parent_ptr)) {
      PTResult *linked_pts =
          processBasePointer(callee_point_to_item_parent_ptr);
      curr_output_pts =
          curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
      curr_output_pts->add_derived_target(interface_cond, linked_pts,
                                          callee_point_to_item_offset);
    } else if (escape_object_map.count(callee_point_to_item_parent_ptr)) {
      // Escaped_obj from callee
      MemObject *curr_obj = escape_object_map[callee_point_to_item_parent_ptr];
      curr_output_pts =
          curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
      curr_output_pts->add_target(interface_cond, curr_obj,
                                  callee_point_to_item_offset);
    } else {
      // The point-to object is from the analyzed function (caller function)
      if (!callee_func_arg.count(callee_point_to_item_parent_ptr))
        continue;

      auto &callee_arg_vals = callee_func_arg[callee_point_to_item_parent_ptr];

      if (!callee_arg_vals.empty()) {
        curr_output_pts =
            curr_output_pts ? curr_output_pts : findPTResult(curr_output, true);
        visited.emplace(curr_output_pts);
      }
      for (auto &arg_point_to : callee_arg_vals) {
        path_cond_t pointer_val_cond = arg_point_to.cond;
        Value *pointer = arg_point_to.val;

        PTResult *linked_pts = processBasePointer(pointer);
        path_cond_t final_cond_point_to =
            findOrCreateAndRegion(pointer_val_cond, interface_cond);
        curr_output_pts->add_derived_target(final_cond_point_to, linked_pts,
                                            callee_point_to_item_offset);
      }
    }
  }
}

void IntraLotusAA::linkOutputValues(
    OutputItem *output, Value *curr_output, size_t idx,
    map<Value *, MemObject *, llvm_cmp> &escape_object_map,
    func_arg_t &callee_func_arg, Instruction *callsite,
    std::unordered_map<PTResult *, PTResultIterator> &pt_result_cache,
    path_cond_t pre_cond) {

  if (idx == 0) {
    // idx=0 means that the real return value, which do not need special linkage
    return;
  }

  AccessPath output_info = output->getSymbolicInfo();
  Value *output_parent = output_info.getParentPtr();
  int64_t output_offset = output_info.getOffset();

  if (escape_object_map.count(output_parent)) {
    // Escaped_obj from callee
    MemObject *curr_obj = escape_object_map[output_parent];
    ObjectLocator *locator = curr_obj->findLocator(output_offset, true);
    locator->storeValue(curr_output, callsite, getEmptyCond(),
                        output->getFuncLevel() + 1);
  } else {
    if (!callee_func_arg.count(output_parent) &&
        isa<GlobalValue>(output_parent)) {
      if (!pre_cond)
        pre_cond = getEmptyCond();
      callee_func_arg[output_parent].push_back(
          mem_value_item_t(pre_cond, nullptr, output_parent));
    }

    if (!callee_func_arg.count(output_parent))
      return;

    auto &callee_arg_vals = callee_func_arg[output_parent];

    if (callee_arg_vals.empty() && isa<GlobalValue>(output_parent)) {
      if (!pre_cond)
        pre_cond = getEmptyCond();
      mem_value_item_t global_value(pre_cond, nullptr, output_parent);
      callee_arg_vals.push_back(global_value);
    }

    for (auto &arg_point_to : callee_arg_vals) {
      path_cond_t pointer_val_cond = arg_point_to.cond;
      Value *pointer = arg_point_to.val;
      if (pointer == LocValue::FREE_VARIABLE) {
        continue;
      }

      PTResult *pt_res = findPTResult(pointer);
      if (pt_res == nullptr) {
        if (isa<Argument>(pointer)) {
          Argument *parent_value_to_arg = dyn_cast<Argument>(pointer);
          pt_res = processArg(parent_value_to_arg);
        } else if (isa<GlobalValue>(pointer)) {
          GlobalValue *global = dyn_cast<GlobalValue>(pointer);
          pt_res = processGlobal(global);
        } else {
          continue;
        }
      }

      if (!pt_result_cache.count(pt_res)) {
        PTResultIterator pt_iter(pt_res, this);
        pt_result_cache.emplace(pt_res, std::move(pt_iter));
      }

      for (auto &pt_item : pt_result_cache.at(pt_res)) {
        ObjectLocator *loc = pt_item.first;
        path_cond_t pt_cond = pt_item.second;
        ObjectLocator *revised_locator = loc->offsetBy(output_offset);
        path_cond_t final_cond_val =
            findOrCreateAndRegion(pointer_val_cond, pt_cond);
        revised_locator->storeValue(curr_output, callsite, final_cond_val,
                                    output->getFuncLevel() + 1);
      }
    }
  }
}

void IntraLotusAA::processCalleeOutput(
    std::vector<OutputItem *> &callee_output,
    set<MemObject *, mem_obj_cmp> &callee_escape, Instruction *callsite,
    Function *callee, path_cond_t pre_cond) {

  auto &func_arg_all = func_arg[callsite];

  if (!func_arg_all.count(callee)) {
    // Inputs for callee function is not processed
    return;
  }

  func_arg_t &callee_func_arg = func_arg_all[callee];

  // (1) Create pseudo-nodes for return value and the side-effect outputs
  std::vector<Value *> &out_values =
      createPseudoOutputNodes(callee_output, callsite, callee);

  // (2) Create the objects that escape to this caller function
  map<Value *, MemObject *, llvm_cmp> escape_object_map;
  createEscapedObjects(callee_escape, callsite, callee, escape_object_map);

  // (3) Link the point-to results and values for each output
  std::set<PTResult *> visited;
  std::unordered_map<PTResult *, PTResultIterator> pt_result_cache;

  for (size_t idx = 0; idx < callee_output.size(); idx++) {
    OutputItem *output = callee_output[idx];
    Value *curr_output = out_values[idx];

    // Link the point-to results for pseudo outputs
    linkOutputPointsToResults(output, curr_output, escape_object_map,
                              callee_func_arg, callsite, callee, visited);

    // Cache PT result iterators
    for (PTResult *visited_item : visited) {
      if (!pt_result_cache.count(visited_item)) {
        PTResultIterator iter(visited_item, this);
        pt_result_cache.emplace(visited_item, std::move(iter));
      }
    }

    // Link the value
    linkOutputValues(output, curr_output, idx, escape_object_map,
                     callee_func_arg, callsite, pt_result_cache, pre_cond);
  }
}

void IntraLotusAA::cacheFunctionCallInfo() {
  if (func_obj)
    return;

  func_obj = newObject(nullptr);
  ObjectLocator *loc = func_obj->findLocator(0, true);

  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      if (CallBase *call = dyn_cast<CallBase>(&inst)) {
        if (Function *called = call->getCalledFunction()) {
          if (called->isIntrinsic())
            continue;
        }
        loc->storeValue(call, call, getEmptyCond(), 0);
      }
    }
  }
}
