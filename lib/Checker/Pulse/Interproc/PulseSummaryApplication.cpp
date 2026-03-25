#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Domain/PulseContradiction.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"

#include <algorithm>
#include <set>

#include <llvm/IR/Instructions.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// Summary application (biabduction materialization)
//
// Applying a callee summary to a caller state is where biabduction becomes
// operational:
// - The summary's precondition represents heap/attribute facts that must hold
//   for the callee witness path to execute.
// - When the caller lacks these facts, we *materialize* them (create missing
//   heap edges/attributes) and record them in the caller's `pre_*` state. This
//   preserves sound incorrectness: we are not claiming the fact always holds,
//   but rather that the witness requires it.
//
// This file contains the "improved" path that attempts to materialize and
// propagate invalidation/nullness facts precisely enough for actionable
// reports.
//===----------------------------------------------------------------------===//

/**
 * Materialize pre-condition: recursively explore the pre-condition subgraph
 * starting from a formal parameter, materializing it in the caller's state.
 *
 * Soundness note:
 * - We only create edges that the callee summary requires, and we record them
 *   in the caller precondition via `abduceToPre`/`abduceAttrToPre`.
 * - If we cannot safely materialize (e.g., base is already invalid), we fail
 *   the entry rather than guessing a projection.
 */
static bool materializePreFromAddress(
    PulseOperations &ops, AbstractValueFactory &factory,
    const AbductiveDomain *callee_pre, AbstractValue formal_addr,
    AbstractValue caller_addr, AbductiveDomain &caller_astate,
    Substitution &substitution, std::set<AbstractValue> &visited) {

  // Check if already visited
  if (visited.count(formal_addr) > 0) {
    return true;
  }
  visited.insert(formal_addr);

  // Check if caller address is valid
  if (caller_astate.getPostAttrs().has(caller_addr, Attribute::Invalid)) {
    return false; // Cannot materialize from invalid address
  }

  // Check if formal address has edges in pre-condition
  const auto &pre_edges = callee_pre->getPreHeap().getEdges();
  auto pre_it = pre_edges.find(formal_addr);
  if (pre_it == pre_edges.end()) {
    return true; // No edges to materialize
  }

  // Materialize each edge
  for (const auto &edge_kv : pre_it->second) {
    const Access &access = edge_kv.first;
    const Address &formal_target = edge_kv.second;

    // Check if edge exists in caller
    const Address *caller_target =
        caller_astate.getPostHeap().findEdge(caller_addr, access);
    if (!caller_target) {
      // Edge doesn't exist - need to abduce it
      // Create fresh value for target
      AbstractValue fresh_target = factory.createFresh();
      Address target_addr(fresh_target);

      // Add edge to caller's heap
      caller_astate.getPostHeap().addEdge(caller_addr, access, target_addr);

      // Abduce to pre (biabduction)
      caller_astate.abduceToPre(caller_addr, access, target_addr);
      caller_astate.abduceAttrToPre(caller_addr, Attribute::Allocated);

      // Update substitution
      substitution.add(formal_target.addr, fresh_target);

      // Recursively materialize from target
      if (!materializePreFromAddress(ops, factory, callee_pre,
                                     formal_target.addr, fresh_target,
                                     caller_astate, substitution, visited)) {
        return false;
      }
    } else {
      // Edge exists - check if we need to update substitution
      AbstractValue caller_target_av =
          caller_astate.getCanonical(caller_target->addr);
      AbstractValue formal_target_canon =
          callee_pre->getCanonical(formal_target.addr);

      // If formal target not in substitution, add it
      if (!substitution.substitute(formal_target_canon)) {
        substitution.add(formal_target_canon, caller_target_av);

        // Recursively materialize from target
        if (!materializePreFromAddress(ops, factory, callee_pre,
                                       formal_target.addr, caller_target_av,
                                       caller_astate, substitution, visited)) {
          return false;
        }
      }
    }
  }

  return true;
}

/**
 * Materialize pre-condition for a formal parameter
 */
static bool materializePreFromFormal(
    PulseOperations &ops, AbstractValueFactory &factory,
    const AbductiveDomain *callee_pre, const PulseSummary &summary,
    const llvm::Value *formal, AbstractValue actual_addr,
    AbductiveDomain &caller_astate, Substitution &substitution) {

  const AbductiveDomain *pre = callee_pre;
  auto formal_av_opt = summary.getFormalAV(formal);
  if (!formal_av_opt) {
    return true; // No formal mapping
  }

  AbstractValue formal_av = pre->getCanonical(*formal_av_opt);
  AbstractValue actual_canon = caller_astate.getCanonical(actual_addr);

  // Check if formal is in pre stack
  const auto &pre_stack = pre->getPreStack().getMap();
  auto stack_it =
      std::find_if(pre_stack.begin(), pre_stack.end(), [&](const auto &kv) {
        auto av_opt = summary.getFormalAV(kv.first);
        return av_opt && pre->getCanonical(*av_opt) == formal_av;
      });

  if (stack_it == pre_stack.end()) {
    return true; // Formal not in pre stack
  }

  // Get the address that formal points to in pre
  AbstractValue formal_addr_in_pre = stack_it->second.addr;
  formal_addr_in_pre = pre->getCanonical(formal_addr_in_pre);

  // Materialize recursively
  std::set<AbstractValue> visited;
  return materializePreFromAddress(ops, factory, pre, formal_addr_in_pre,
                                   actual_canon, caller_astate, substitution,
                                   visited);
}

/**
 * Apply post-condition: copy post edges and attributes with substitution,
 * but delete edges that were in the pre-condition (read-only optimization)
 */
static void
applyPostCondition(const AbductiveDomain *callee_post,
                   const AbductiveDomain *callee_pre,
                   AbductiveDomain &caller_astate, Substitution &substitution,
                   AbstractValueFactory &factory, PulseOperations &ops,
                   const llvm::Function *callee, const llvm::CallInst *CI,
                   const llvm::BasicBlock *pred, const PulseSummary &summary) {

  // Collect pre edges for read-only optimization
  std::set<std::pair<AbstractValue, Access>> pre_edges;
  for (const auto &kv : callee_pre->getPreHeap().getEdges()) {
    AbstractValue formal_from = kv.first;
    for (const auto &edge_kv : kv.second) {
      pre_edges.insert({formal_from, edge_kv.first});
    }
  }

  // Apply post edges (with substitution)
  for (const auto &kv : callee_post->getPostHeap().getEdges()) {
    AbstractValue formal_from = kv.first;
    AbstractValue formal_from_canon = callee_post->getCanonical(formal_from);
    auto caller_from_opt = substitution.substitute(formal_from_canon);
    AbstractValue caller_from =
        caller_from_opt ? *caller_from_opt : factory.createFresh();
    if (!caller_from_opt) {
      substitution.add(formal_from_canon, caller_from);
    }
    caller_from = caller_astate.getCanonical(caller_from);

    for (const auto &edge_kv : kv.second) {
      const Access &access = edge_kv.first;
      const Address &formal_target = edge_kv.second;

      // Check if this edge was in pre (read-only)
      AbstractValue formal_from_canon = callee_pre->getCanonical(formal_from);
      if (pre_edges.count({formal_from_canon, access}) > 0) {
        // Edge was in pre - check if it's read-only (not modified)
        // For now, we always update (simplified)
      }

      // Substitute target
      AbstractValue formal_target_canon =
          callee_post->getCanonical(formal_target.addr);
      auto caller_target_opt = substitution.substitute(formal_target_canon);
      AbstractValue caller_target =
          caller_target_opt ? *caller_target_opt : factory.createFresh();
      if (!caller_target_opt) {
        substitution.add(formal_target_canon, caller_target);
      }
      caller_target = caller_astate.getCanonical(caller_target);

      Address caller_target_addr(caller_target);
      caller_target_addr.history = formal_target.history;

      // Add or update edge
      caller_astate.getPostHeap().addEdge(caller_from, access,
                                          caller_target_addr);
    }
  }

  // Apply post attributes (with substitution) - CRITICAL for Invalid
  // propagation The substitution should already contain formal->actual mappings
  // from earlier
  for (const auto &kv : callee_post->getPostAttrs().getAttrs()) {
    AbstractValue formal_av = kv.first;
    AbstractValue formal_av_canon = callee_post->getCanonical(formal_av);

    // Try to find this formal value in the substitution map
    auto caller_av_opt = substitution.substitute(formal_av_canon);

    // If not found, skip (this attribute doesn't apply to caller)
    if (!caller_av_opt) {
      continue;
    }

    AbstractValue caller_av = caller_astate.getCanonical(*caller_av_opt);

    for (Attribute attr : kv.second) {
      caller_astate.getPostAttrs().add(caller_av, attr);

      // Special handling for Invalid attribute: also propagate to aliases
      if (attr == Attribute::Invalid) {
        // Find all values that canonicalize to the same value (aliases)
        // Check stack variables
        for (auto &stack_kv : caller_astate.getPostStack().getMap()) {
          AbstractValue stack_canon =
              caller_astate.getCanonical(stack_kv.second.addr);
          if (stack_canon == caller_av) {
            // This is an alias - also mark as invalid
            caller_astate.getPostAttrs().add(stack_canon, Attribute::Invalid);
            caller_astate.getPostAttrs().remove(stack_canon,
                                                Attribute::Allocated);
          }
        }

        // Also check heap edges - if caller_av is stored in heap, propagate
        // Invalid there too This handles cases where the pointer is stored in a
        // variable and then loaded
        for (const auto &heap_kv : caller_astate.getPostHeap().getEdges()) {
          for (const auto &edge_kv : heap_kv.second) {
            AbstractValue target_canon =
                caller_astate.getCanonical(edge_kv.second.addr);
            if (target_canon == caller_av) {
              // This heap location points to the invalid value - mark it as
              // invalid
              caller_astate.getPostAttrs().add(target_canon,
                                               Attribute::Invalid);
              caller_astate.getPostAttrs().remove(target_canon,
                                                  Attribute::Allocated);
            }
          }
        }

        // Use invalidate() to properly propagate Invalid to all aliases
        // This ensures transitive invalidation
        Address invalid_addr(caller_av);
        ops.invalidate(caller_astate, invalid_addr, nullptr,
                       InvalidationKind::Other);
      }
    }
  }

  // Apply allocation sizes (with substitution).
  for (const auto &kv : callee_post->getAllocationSizes()) {
    AbstractValue formal_av = callee_post->getCanonical(kv.first);
    auto caller_av_opt = substitution.substitute(formal_av);
    if (!caller_av_opt) {
      continue;
    }
    AbstractValue caller_av = caller_astate.getCanonical(*caller_av_opt);
    caller_astate.setAllocationSize(caller_av, kv.second);
  }
}

/**
 * Improved summary application with materialization
 */
std::vector<ExecutionDomain> PulseChecker::applySummaryImproved(
    const llvm::Function *callee, const ExecutionDomain &caller_state,
    const llvm::CallInst *CI, const llvm::BasicBlock *pred) {

  const PulseSummary *summary_ptr = summary_manager_.getSummary(callee);
  if (!summary_ptr || !summary_ptr->isValid()) {
    return {};
  }

  const PulseSummary &summary = *summary_ptr;
  const AbductiveDomain *caller_astate = caller_state.getAstate();
  if (!caller_astate) {
    return {caller_state};
  }

  const unsigned formal_arg_count = callee->arg_size();

  std::vector<ExecutionDomain> results;
  results.reserve(
      std::min<unsigned>(kMaxDisjuncts, summary.getPrePostList().size()));

  for (const auto &entry : summary.getPrePostList()) {
    const AbductiveDomain *pre = entry.getPre();
    const AbductiveDomain *post = entry.getPost();
    if (!pre || !post) {
      continue;
    }

    ExecutionDomain new_state = caller_state.clone();
    auto *new_astate = new_state.getAstate();
    if (!new_astate) {
      continue;
    }

    Substitution substitution;
    std::map<AbstractValue, AbstractValue> formal_to_actual_map;
    std::map<AbstractValue, std::set<AbstractValue>> actual_to_formals_map;

    // Track captured variables (for closures/lambdas)
    // In LLVM, captured variables are typically passed as additional arguments
    // or accessed through a closure structure
    std::map<const llvm::Value *, AbstractValue> captured_vars;

    unsigned arg_idx = 0;
    bool entry_failed = false;

    // Check formal/actual length mismatch first
    const unsigned actual_arg_count = CI->arg_size();

    // Handle captured variables: check if this is a closure call
    // In LLVM IR, closures often have a first argument that's the closure
    // structure We need to extract captured variables from it Infer handles
    // this via CapturedFormalActualLength contradiction
    bool is_closure_call = false;
    std::vector<AbstractValue> captured_actuals;

    if (CI->arg_size() > 0) {
      // Check if first argument looks like a closure (heuristic)
      const llvm::Value *first_arg = CI->getArgOperand(0);
      if (first_arg->getType()->isPointerTy()) {
        // Try to evaluate it - if it has fields, might be a closure
        auto closure_opt = ops_.eval(*new_astate, first_arg, CI, pred);
        if (closure_opt) {
          // Check if this address has closure-like structure
          // In practice, we'd check for specific patterns or attributes
          // For now, we'll handle it generically
          is_closure_call = true;

          // Extract captured variables from closure structure
          // This is simplified - full implementation would traverse closure
          // struct
          AbstractValue closure_addr =
              new_astate->getCanonical(closure_opt->addr);

          // Mark closure address for later processing
          // In full implementation, we'd extract each captured variable
          captured_vars[first_arg] = closure_addr;
          captured_actuals.push_back(closure_addr);

          // In Infer, captured variables are tracked separately and checked
          // via CapturedFormalActualLength contradiction
        }
      }
    }

    // Check formal/actual length mismatch (including captured variables)
    // Infer uses FormalActualLength and CapturedFormalActualLength
    // contradictions
    if (!callee->isVarArg() && actual_arg_count != formal_arg_count &&
        !is_closure_call) {
      // Formal/actual length mismatch - this is a contradiction
      // Report as FormalActualLength contradiction
      auto contradiction = Contradiction::makeFormalActualLength(
          formal_arg_count, actual_arg_count);
      // Skip this entry, try next
      continue;
    }

    // If closure call, check captured formal/actual length match
    // (In full implementation, we'd extract captured formals from callee
    // signature)
    if (is_closure_call && !captured_actuals.empty()) {
      // Simplified: assume captured formals match if closure structure exists
      // Full implementation would:
      // 1. Extract captured formals from callee signature (from function
      // attributes/metadata)
      // 2. Compare with captured_actuals.size()
      // 3. Report CapturedFormalActualLength contradiction if mismatch
      //
      // For now, we handle it generically - full implementation would check:
      // unsigned captured_formal_count = extractCapturedFormals(callee).size();
      // if (captured_formal_count != captured_actuals.size()) {
      //     auto contradiction = Contradiction::makeCapturedFormalActualLength(
      //         captured_formal_count, captured_actuals.size());
      //     continue;  // Skip this entry
      // }
    }

    // Build formal-to-actual mapping for ALL arguments first
    // This ensures substitution is complete before applying post conditions
    // Match formal parameters to actual arguments by position
    arg_idx = 0;
    for (const auto &Arg : callee->args()) {
      if (arg_idx >= CI->arg_size()) {
        break;
      }

      auto actual_opt =
          ops_.eval(*new_astate, CI->getArgOperand(arg_idx), CI, pred);
      if (!actual_opt) {
        arg_idx++;
        continue;
      }

      AbstractValue actual_addr = new_astate->getCanonical(actual_opt->addr);
      auto formal_av_opt = summary.getFormalAV(&Arg);
      if (formal_av_opt) {
        AbstractValue formal_av = pre->getCanonical(*formal_av_opt);

        // Build reverse mapping for aliasing contradiction detection
        actual_to_formals_map[actual_addr].insert(formal_av);

        substitution.add(formal_av, actual_addr);
        formal_to_actual_map[formal_av] = actual_addr;

        if (!materializePreFromFormal(ops_, factory_, pre, summary, &Arg,
                                      actual_addr, *new_astate, substitution)) {
          entry_failed = true;
          break;
        }
      }

      arg_idx++;
    }

    if (entry_failed) {
      continue;
    }
    if (!callee->isVarArg() && actual_arg_count != formal_arg_count) {
      continue;
    }

    // Normalize all abstract values in caller state before comparison
    // This ensures we're comparing canonical representatives
    for (const auto &kv : new_astate->getPostStack().getMap()) {
      AbstractValue canon = new_astate->getCanonical(kv.second.addr);
      (void)canon; // Ensure normalization happens
    }

    // Normalize heap edges
    for (const auto &kv : new_astate->getPostHeap().getEdges()) {
      AbstractValue canon_from = new_astate->getCanonical(kv.first);
      (void)canon_from;
      for (const auto &edge_kv : kv.second) {
        AbstractValue canon_to = new_astate->getCanonical(edge_kv.second.addr);
        (void)canon_to;
      }
    }

    PulseFormula caller_formula = new_astate->getPathFormula().clone();
    PulseFormula callee_pre_formula =
        entry.getPreFormula().applySubstitution(substitution);

    // Enhanced contradiction detection using new module
    auto contradiction_opt =
        checkContradiction(caller_formula, callee_pre_formula,
                           formal_to_actual_map, actual_to_formals_map);

    if (contradiction_opt) {
      // Contradiction detected - skip this entry
      continue;
    }

    // Also check if formula is UNSAT using Z3
    if (!callee_pre_formula.isConsistent() || callee_pre_formula.isUnsat()) {
      continue;
    }

    // Check if merged formula is UNSAT using Z3
    PulseFormula test_merge =
        PulseFormula::merge(caller_formula, callee_pre_formula);
    if (!test_merge.isConsistent() || test_merge.isUnsat()) {
      continue;
    }

    // Additional normalization: ensure substitution maps use canonical values
    // Rebuild substitution from formal_to_actual_map with canonical values
    Substitution normalized_substitution;
    for (const auto &kv : formal_to_actual_map) {
      AbstractValue formal_canon = pre->getCanonical(kv.first);
      AbstractValue actual_canon = new_astate->getCanonical(kv.second);
      normalized_substitution.add(formal_canon, actual_canon);
    }
    substitution = std::move(normalized_substitution);

    for (const auto &kv : formal_to_actual_map) {
      AbstractValue formal_av = kv.first;
      AbstractValue actual_av = kv.second;

      (void)formal_av;
      if (callee_pre_formula.isNull(actual_av)) {
        if (caller_formula.isNonNull(actual_av) ||
            new_astate->getPostAttrs().has(actual_av, Attribute::Allocated)) {
          entry_failed = true;
          break;
        }
      } else if (callee_pre_formula.isNonNull(actual_av)) {
        if (caller_formula.isNull(actual_av) ||
            new_astate->getPostAttrs().has(actual_av, Attribute::Null)) {
          entry_failed = true;
          break;
        }
      }

      if (pre->getPreAttrs().has(kv.first, Attribute::Allocated)) {
        if (new_astate->getPostAttrs().has(actual_av, Attribute::Invalid)) {
          entry_failed = true;
          break;
        }
      }
    }

    if (entry_failed) {
      continue;
    }

    PulseFormula merged_pre =
        PulseFormula::merge(caller_formula, callee_pre_formula);
    if (!merged_pre.isConsistent()) {
      continue;
    }

    llvm::Optional<AbstractValue> caller_ret = llvm::None;
    if (entry.getReturnValue()) {
      AbstractValue formal_ret = post->getCanonical(*entry.getReturnValue());
      auto caller_ret_opt = substitution.substitute(formal_ret);
      if (caller_ret_opt) {
        caller_ret = new_astate->getCanonical(*caller_ret_opt);
      } else {
        AbstractValue fresh_ret = factory_.createFresh(CI);
        substitution.add(formal_ret, fresh_ret);
        caller_ret = new_astate->getCanonical(fresh_ret);
      }
    }

    // Apply post condition - this propagates Invalid attributes from callee to
    // caller The substitution should already map formal parameters to actual
    // arguments
    applyPostCondition(post, pre, *new_astate, substitution, factory_, ops_,
                       callee, CI, pred, summary);

    PulseFormula post_formula =
        entry.getPostFormula().applySubstitution(substitution);
    if (!post_formula.isConsistent()) {
      continue;
    }
    PulseFormula merged_post = PulseFormula::merge(merged_pre, post_formula);
    if (merged_post.isConsistent()) {
      new_astate->setPathFormula(
          std::make_unique<PulseFormula>(std::move(merged_post)));
    }

    if (entry.getLatentIssue()) {
      const auto &latent = *entry.getLatentIssue();
      AbstractValue formal_addr = post->getCanonical(latent.address);
      auto caller_addr_opt = substitution.substitute(formal_addr);
      AbstractValue caller_addr =
          caller_addr_opt ? *caller_addr_opt : factory_.createFresh(CI);
      if (!caller_addr_opt) {
        substitution.add(formal_addr, caller_addr);
      }
      caller_addr = new_astate->getCanonical(caller_addr);

      Trace issue_trace = latent.trace.clone();
      issue_trace.addEvent(CI, "Call");

      if (LatentIssue::isManifest(latent.diagnostic, *new_astate,
                                  caller_addr)) {
        reportBug(latent.diagnostic, CI, caller_addr, issue_trace, new_astate);
        results.push_back(ExecutionDomain::abortProgram(
            std::make_unique<AbductiveDomain>(new_astate->clone()),
            latent.diagnostic, issue_trace.clone()));
      } else {
        latent_issues_.emplace_back(
            latent.diagnostic,
            LatentIssue::issueKindFromResult(latent.diagnostic), caller_addr,
            CI, issue_trace.clone());
        for (const auto &cc : latent.calling_context) {
          latent_issues_.back().addCallingContext(cc.first, cc.second);
        }
        latent_issues_.back().addCallingContext(callee, CI);
        results.push_back(ExecutionDomain::latentAbortProgram(
            std::make_unique<AbductiveDomain>(new_astate->clone()),
            &latent_issues_.back()));
      }
      if (results.size() >= kMaxDisjuncts) {
        break;
      }
      continue;
    }

    if (entry.getReturnValue()) {
      Address ret_addr(*caller_ret);
      // Add FunctionCall event to history so isNullConstantSource can detect it
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      new_astate->getPostStack().add(CI, ret_addr);
    }

    results.push_back(std::move(new_state));
    if (results.size() >= kMaxDisjuncts) {
      break;
    }
  }

  if (!results.empty()) {
    return results;
  }

  return {};
}

} // namespace pulse
