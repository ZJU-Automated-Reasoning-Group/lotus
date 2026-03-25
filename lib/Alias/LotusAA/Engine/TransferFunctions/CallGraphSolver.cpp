/// @file CallGraphSolver.cpp
/// @brief Call graph construction and indirect call resolution using pointer
/// analysis
///
/// This file implements **context-sensitive call graph construction** for
/// LotusAA, resolving indirect function calls (function pointers) using the
/// results of pointer analysis.
///
/// **Key Responsibilities:**
/// 1. **Indirect Call Resolution**: Determine possible callee targets for
/// function pointers
/// 2. **Call Graph Summaries**: Build compact summaries of which functions may
/// be called
/// 3. **Inter-procedural Propagation**: Inline summary information through call
/// chains
/// 4. **Visualization**: Print resolved call graphs for debugging
///
/// **Algorithm Overview:**
/// ```
/// For each indirect call site:
///   1. Track function pointer value through program
///   2. Resolve to concrete function values via points-to
///   3. Handle returns from callees (functions returned by functions)
///   4. Handle arguments (function pointers passed as parameters)
///   5. Build input/output call graph summaries
/// ```
///
/// **Call Graph Summaries:**
/// - **Input Summaries**: Maps arguments to possible function targets
/// - **Output Summaries**: Maps return positions to possible function targets
/// - Used for context-sensitive interprocedural analysis
///
/// **Command-line Options:**
/// - `--lotus-print-cg-details`: Detailed call graph resolution diagnostics
///
/// @see functionPointerResults.h for storing resolved call graph
/// @see trackPtrRightValue() for value tracking implementation

#include "Alias/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace std;

static cl::opt<bool>
    lotus_print_cg_details("lotus-print-cg-details",
                           cl::desc("Print detailed CG resolution info"),
                           cl::init(false), cl::Hidden);

/// Resolves a value to the set of functions it may represent.
///
/// This is the core resolution function that tracks a value through the program
/// to determine which function(s) it may represent at runtime. Handles:
/// - Direct function pointers
/// - Functions returned from calls
/// - Functions passed through arguments
///
/// @param val The value to resolve (may be function, call result, argument,
/// etc.)
/// @param target Output set populated with resolved function targets
///
/// **Resolution Cases:**
/// 1. **Direct Function**: `val` is a Function* → add to target set
/// 2. **Call Return**: `val` is CallBase → query callee's output summary
/// 3. **Argument**: `val` is Argument → mark as needing inter-procedural
/// resolution
///
/// **Inter-procedural Handling:**
/// For arguments, adds to `input_cg_summary` so callers can provide concrete
/// values.
///
/// @see computeCG() for the main call graph computation algorithm
/// @see trackPtrRightValue() for value tracking implementation
void IntraLotusAA::resolveCallValue(Value *val, cg_result_t &target,
                                    path_cond_t pre_cond) {
  if (!pre_cond)
    pre_cond = getEmptyCond();

  mem_value_t resolved_tmp;
  trackPtrRightValue(val, resolved_tmp);

  for (auto &item : resolved_tmp) {
    Value *resolved_val = item.val;

    if (Function *func = dyn_cast<Function>(resolved_val)) {
      // Direct function pointer
      path_cond_t final_cond = findOrCreateAndRegion(item.cond, pre_cond);
      if (target.count(func)) {
        target[func] = findOrCreateOrRegion(target[func], final_cond);
      } else {
        target[func] = final_cond;
      }
    } else if (isa<CallBase>(resolved_val) ||
               func_pseudo_ret_cache.count(resolved_val)) {
      // Function returned from call or pseudo-output.
      Instruction *callsite_inst = nullptr;
      int output_idx = 0;

      if (CallBase *call = dyn_cast<CallBase>(resolved_val)) {
        callsite_inst = call;
      } else {
        auto ret_it = func_pseudo_ret_cache.find(resolved_val);
        assert(ret_it != func_pseudo_ret_cache.end() &&
               "Unexpected pseudo return in CG resolution");
        callsite_inst = ret_it->second.first;
        output_idx = ret_it->second.second;
      }

      CallBase *item_call = dyn_cast_or_null<CallBase>(callsite_inst);
      if (!item_call || output_idx < 0)
        continue;

      Function *called_func = item_call->getCalledFunction();
      if (!called_func)
        continue;

      IntraLotusAA *callee_PTG = lotus_aa->getPtGraph(called_func);
      if (!callee_PTG || callee_PTG->is_considered_as_library ||
          static_cast<size_t>(output_idx) >= callee_PTG->output_cg_summary.size())
        continue;

      cg_result_t &out_values = callee_PTG->output_cg_summary[output_idx];
      for (auto &value_item : out_values) {
        path_cond_t imported_cond =
            importPathCond(value_item.second, callsite_inst, called_func);
        path_cond_t final_cond =
            findOrCreateAndRegion(imported_cond, pre_cond);
        if (target.count(value_item.first)) {
          target[value_item.first] =
              findOrCreateOrRegion(target[value_item.first], final_cond);
        } else {
          target[value_item.first] = final_cond;
        }
      }
    } else if (Argument *resolved_arg = dyn_cast<Argument>(resolved_val)) {
      if (resolved_arg->getParent() || inputs.count(resolved_val)) {
        // Real argument or pseudo-argument
        map<cg_result_t *, path_cond_t> &in_summary =
            input_cg_summary[resolved_arg];
        if (in_summary.count(&target)) {
          in_summary[&target] =
              findOrCreateOrRegion(in_summary[&target], pre_cond);
        } else {
          in_summary[&target] = pre_cond;
        }
      }
    }
  }
}

/// Computes the call graph for this function, resolving all indirect calls.
///
/// This is the main entry point for call graph analysis. It:
/// 1. Resolves indirect call sites to determine possible callees
/// 2. Inlines callee input summaries to resolve function pointer parameters
/// 3. Builds output summaries for use by callers
/// 4. Handles recursive and high-order function scenarios
///
/// **Algorithm:**
/// ```
/// for each call site:
///   for each possible callee:
///     // Inline callee's input CG summaries
///     for each arg that is function pointer in callee:
///       resolve arg value in caller context
///       add to callee's input summary targets
///   // Resolve call value itself
///   resolveCallValue(called_operand, cg_resolve_result[call])
///
/// // Build output summaries
/// for each output (return value, side-effects):
///   resolve output values to functions
///   store in output_cg_summary
/// ```
///
/// **Configuration:**
/// - `lotus_restrict_inline_depth`: Controls interprocedural depth
/// - `lotus_restrict_cg_size`: Limits number of indirect targets processed
///
/// @note Sets is_CG_computed = true on completion
/// @see resolveCallValue() for per-value resolution logic
void IntraLotusAA::computeCG() {
  if (is_considered_as_library || !is_PTA_computed || is_CG_computed)
    return;

  Function *func = analyzed_func;

  // Resolve indirect calls
  for (BasicBlock *bb : topBBs) {
    for (Instruction &inst : *bb) {
      if (CallBase *call = dyn_cast<CallBase>(&inst)) {
        // Process both direct and indirect calls
        Function *base_func = func;
        auto *callees = lotus_aa->getCallees(base_func, call);

        if (callees && IntraLotusAAConfig::lotus_restrict_inline_depth != 0) {
          // Inline input summaries from callees
          int callee_idx = -1;
          for (auto &callee_item : *callees) {
            callee_idx++;
            Function *callee = callee_item.first;
            if (callee_idx >= IntraLotusAAConfig::lotus_restrict_cg_size)
              break;

            if (!callee || !isPointerAnalysisCallsiteCompatible(call, callee) ||
                lotus_aa->isBackEdge(base_func, callee)) {
              continue;
            }

            IntraLotusAA *callee_PTG = nullptr;
            func_arg_t *caller_args = nullptr;

            if (func_arg.count(call) && func_arg[call].count(callee)) {
              callee_PTG = lotus_aa->getPtGraph(callee);
              if (callee_PTG && !callee_PTG->is_considered_as_library) {
                caller_args = &func_arg[call][callee];
              }
            } else if (!IntraLotusAAConfig::lotus_disable_thread_heuristic &&
                       callee->getName() == "pthread_create" &&
                       call->arg_size() == 4 &&
                       isa<Function>(call->getArgOperand(2)) &&
                       call->getArgOperand(3)->getType()->isPointerTy() &&
                       thread_arg.count(call)) {
              Function *thread_func =
                  dyn_cast<Function>(call->getArgOperand(2));
              callee_PTG = lotus_aa->getPtGraph(thread_func);
              if (callee_PTG && !callee_PTG->is_considered_as_library) {
                caller_args = &thread_arg[call];
              }
            }

            if (callee_PTG && caller_args) {

              // Process input CG summaries
              map<Argument *, map<cg_result_t *, path_cond_t>, llvm_cmp>
                  &callee_input_cg_summary = callee_PTG->input_cg_summary;

              for (auto &arg_summary_pair : callee_input_cg_summary) {
                Argument *callee_arg = arg_summary_pair.first;
                map<cg_result_t *, path_cond_t> &callee_arg_summary =
                    arg_summary_pair.second;

                if (!caller_args->count(callee_arg))
                  continue;

                mem_value_t &caller_arg_values = (*caller_args)[callee_arg];

                for (auto &callee_summary_item : callee_arg_summary) {
                  cg_result_t *inline_target = callee_summary_item.first;
                  path_cond_t inline_pre_cond = callee_summary_item.second;

                  for (auto &caller_arg_value : caller_arg_values) {
                    path_cond_t imported_inline_cond =
                        importPathCond(inline_pre_cond, call, callee);
                    path_cond_t final_cond = findOrCreateAndRegion(
                        caller_arg_value.cond, imported_inline_cond);
                    Value *caller_arg_value_val = caller_arg_value.val;
                    resolveCallValue(caller_arg_value_val, *inline_target,
                                     final_cond);
                  }
                }
              }
            }
          }
        }

        // Resolve the call value itself
        Value *called_value = call->getCalledOperand();
        resolveCallValue(called_value, cg_resolve_result[call], getEmptyCond());
      }
    }
  }

  // Compute output CG summary
  if (IntraLotusAAConfig::lotus_restrict_inline_depth != 0) {
    size_t output_size = outputs.size();
    output_cg_summary.resize(output_size);

    for (size_t idx = 0; idx < output_size; idx++) {
      OutputItem *output_item = outputs[idx];
      if (!output_item->getType()->isPointerTy())
        continue;

      cg_result_t &target = output_cg_summary[idx];

      auto &src_value_all = output_item->getVal();
      for (auto &src_item : src_value_all) {
        mem_value_t &src = src_item.second;
        for (mem_value_item_t &value_item : src) {
          Value *src_value = value_item.val;
          resolveCallValue(src_value, target, value_item.cond);
        }
      }
    }
  }

  is_CG_computed = true;
}

/// Prints resolved function pointer targets for debugging.
///
/// Displays all indirect call sites in this function along with their
/// resolved targets. Only shows indirect calls (skips direct calls).
///
/// **Output Format:**
/// ```
/// ========== Function Pointers: function_name ==========
///   Call Site: %call = call %fp(...)
///     -> target_func_1
///     -> target_func_2
/// ===============================================
/// ```
///
/// @note Only outputs if there are indirect calls with resolutions
/// @see computeCG() for the resolution algorithm
void IntraLotusAA::showFunctionPointers() {
  bool title_printed = false;

  for (auto &cg_item : cg_resolve_result) {
    Value *call_site = cg_item.first;
    if (CallBase *call = dyn_cast<CallBase>(call_site)) {
      if (call->getCalledFunction())
        continue; // Skip direct calls
    }

    if (!title_printed) {
      outs() << "\n";
      outs() << "========== Function Pointers: " << analyzed_func->getName()
             << " ==========\n";
      title_printed = true;
    }

    outs() << "  Call Site: ";
    call_site->print(outs());
    outs() << "\n";

    cg_result_t &result = cg_item.second;
    for (auto &resolved_item : result) {
      outs() << "    -> " << resolved_item.first->getName() << "\n";
    }
  }

  if (title_printed)
    outs() << "===============================================\n\n";
}
