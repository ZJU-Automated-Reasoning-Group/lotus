#include "Checker/Pulse/Checker/PulseChecker.h"
#include "Checker/Pulse/Checker/PulseCheckerUtils.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"
#include "Checker/Pulse/Report/PulseLogger.h"

#include <map>
#include <set>
#include <vector>

#include <llvm/IR/Instructions.h>

namespace pulse {

namespace {
static std::set<AbstractValue>
computeReachable(const AbductiveDomain &astate, const Heap &heap,
                 const std::set<AbstractValue> &roots) {
  std::set<AbstractValue> reachable = roots;
  std::vector<AbstractValue> worklist(roots.begin(), roots.end());

  while (!worklist.empty()) {
    AbstractValue cur = worklist.back();
    worklist.pop_back();
    auto it = heap.getEdges().find(cur);
    if (it == heap.getEdges().end())
      continue;
    for (const auto &edge_kv : it->second) {
      AbstractValue target = astate.getCanonical(edge_kv.second.addr);
      if (reachable.insert(target).second) {
        worklist.push_back(target);
      }
    }
  }

  return reachable;
}

static AbductiveDomain pruneForSummary(const llvm::Function *F,
                                       const AbductiveDomain &astate,
                                       llvm::Optional<AbstractValue> ret_val) {
  AbductiveDomain pruned = astate.clone();

  // Roots: formals, return value (if any), and globals.
  std::set<AbstractValue> roots;
  for (const auto &Arg : F->args()) {
    if (auto *addr = pruned.getPostStack().find(&Arg)) {
      roots.insert(pruned.getCanonical(addr->addr));
    }
    if (auto *addr = pruned.getPreStack().find(&Arg)) {
      roots.insert(pruned.getCanonical(addr->addr));
    }
  }
  if (ret_val) {
    roots.insert(pruned.getCanonical(*ret_val));
  }
  for (const auto &kv : pruned.getPostAttrs().getAttrs()) {
    if (kv.second.count(Attribute::Global) > 0) {
      roots.insert(pruned.getCanonical(kv.first));
    }
  }

  const std::set<AbstractValue> reachable_post =
      computeReachable(pruned, pruned.getPostHeap(), roots);
  const std::set<AbstractValue> reachable_pre =
      computeReachable(pruned, pruned.getPreHeap(), roots);

  // Prune stacks: keep only formals (and any retained values explicitly).
  {
    Stack new_post;
    Stack new_pre;
    for (const auto &Arg : F->args()) {
      if (auto *addr = pruned.getPostStack().find(&Arg)) {
        if (reachable_post.count(pruned.getCanonical(addr->addr)) > 0) {
          new_post.add(&Arg, *addr);
        }
      }
      if (auto *addr = pruned.getPreStack().find(&Arg)) {
        if (reachable_pre.count(pruned.getCanonical(addr->addr)) > 0) {
          new_pre.add(&Arg, *addr);
        }
      }
    }
    pruned.getPostStack() = std::move(new_post);
    pruned.getPreStack() = std::move(new_pre);
  }

  // Prune heaps.
  {
    Heap new_post;
    for (const auto &kv : pruned.getPostHeap().getEdges()) {
      if (reachable_post.count(pruned.getCanonical(kv.first)) == 0)
        continue;
      AbstractValue from = pruned.getCanonical(kv.first);
      for (const auto &edge_kv : kv.second) {
        AbstractValue to = pruned.getCanonical(edge_kv.second.addr);
        if (reachable_post.count(to) == 0)
          continue;
        new_post.addEdge(from, edge_kv.first, edge_kv.second);
      }
    }
    pruned.getPostHeap() = std::move(new_post);
  }
  {
    Heap new_pre;
    for (const auto &kv : pruned.getPreHeap().getEdges()) {
      if (reachable_pre.count(pruned.getCanonical(kv.first)) == 0)
        continue;
      AbstractValue from = pruned.getCanonical(kv.first);
      for (const auto &edge_kv : kv.second) {
        AbstractValue to = pruned.getCanonical(edge_kv.second.addr);
        if (reachable_pre.count(to) == 0)
          continue;
        new_pre.addEdge(from, edge_kv.first, edge_kv.second);
      }
    }
    pruned.getPreHeap() = std::move(new_pre);
  }

  // Prune attrs.
  {
    AddressAttributes new_post_attrs;
    for (const auto &kv : pruned.getPostAttrs().getAttrs()) {
      if (reachable_post.count(pruned.getCanonical(kv.first)) == 0)
        continue;
      for (Attribute a : kv.second) {
        new_post_attrs.add(pruned.getCanonical(kv.first), a);
      }
    }
    pruned.getPostAttrs() = std::move(new_post_attrs);

    AddressAttributes new_pre_attrs;
    for (const auto &kv : pruned.getPreAttrs().getAttrs()) {
      if (reachable_pre.count(pruned.getCanonical(kv.first)) == 0)
        continue;
      for (Attribute a : kv.second) {
        new_pre_attrs.add(pruned.getCanonical(kv.first), a);
      }
    }
    pruned.getPreAttrs() = std::move(new_pre_attrs);
  }

  // Prune allocation sizes.
  {
    std::map<AbstractValue, uint64_t> sizes;
    for (const auto &kv : pruned.getAllocationSizes()) {
      AbstractValue canon = pruned.getCanonical(kv.first);
      if (reachable_post.count(canon) > 0) {
        sizes.emplace(canon, kv.second);
      }
    }
    pruned.getAllocationSizes() = std::move(sizes);
  }

  pruned.canonicalize();
  return pruned;
}
} // namespace

void PulseChecker::createSummary(
    const llvm::Function *F, const std::vector<ExecutionDomain> &exit_states,
    const std::vector<ExecutionDomain> &latent_exit_states) {
  PulseLogger::debug("Creating summary for " + F->getName().str() + " (" +
                     std::to_string(exit_states.size()) + " exit states, " +
                     std::to_string(latent_exit_states.size()) +
                     " latent states)");
  PulseLogger::incrementCounter("summaries.created");
  if (exit_states.empty() && latent_exit_states.empty())
    return;

  PulseSummary summary(F);

  unsigned latent_added = 0;
  for (const auto &latent_state : latent_exit_states) {
    if (!latent_state.isStopped())
      continue;
    if (!latent_state.isLatentAbortProgram() &&
        !latent_state.isLatentInvalidAccess())
      continue;
    if (summary.getPrePostList().size() >= kMaxDisjuncts)
      break;
    const AbductiveDomain *astate = latent_state.getAstate();
    if (!astate)
      continue;

    auto *issue = latent_state.getStoppedExecution().latent_issue;
    if (!issue)
      continue;

    SummaryEntry::LatentIssueSummary latent;
    latent.diagnostic = issue->getDiagnostic();
    latent.address = astate->getCanonical(issue->getAddress());
    // Avoid cloning trace/calling_context when large to prevent
    // std::length_error and crashes from huge or corrupt vectors (e.g. in
    // write_gauge_info_item).
    const Trace &issue_trace = issue->getTrace();
    const auto &cc = issue->getCallingContext();
    constexpr size_t kMaxTraceEventsForSummary = 1024;
    constexpr size_t kMaxCallingContextForSummary = 1024;
    if (issue_trace.getEvents().size() <= kMaxTraceEventsForSummary &&
        cc.size() <= kMaxCallingContextForSummary) {
      latent.trace = issue_trace.clone();
      latent.calling_context = cc;
    } else {
      latent.trace = Trace();
      latent.calling_context.clear();
    }

    const PulseFormula formula = astate->getPathFormula().clone();
    AbductiveDomain pruned = pruneForSummary(F, *astate, llvm::None);
    auto pre = std::make_unique<AbductiveDomain>(pruned.clone());
    auto post = std::make_unique<AbductiveDomain>(std::move(pruned));
    summary.addPrePost(SummaryEntry(
        std::move(pre), formula.clone(), std::move(post), formula.clone(),
        llvm::None,
        llvm::Optional<SummaryEntry::LatentIssueSummary>(std::move(latent))));
    latent_added++;
  }

  bool has_any_entry = latent_added > 0;
  const unsigned max_normal_entries =
      kMaxDisjuncts > latent_added ? (kMaxDisjuncts - latent_added) : 0u;

  for (const auto &exit_state : exit_states) {
    // Skip abort/latent states (bugs), but process normal ExitProgram
    if (exit_state.isAbortProgram() || exit_state.isLatentAbortProgram() ||
        exit_state.isLatentInvalidAccess() ||
        exit_state.isLatentSpecializedTypeIssue()) {
      continue;
    }
    const AbductiveDomain *astate = exit_state.getAstate();
    if (!astate)
      continue;

    has_any_entry = true;
    const PulseFormula formula = astate->getPathFormula().clone();
    llvm::Optional<AbstractValue> ret_val = llvm::None;
    if (exit_state.isStopped()) {
      ret_val = exit_state.getStoppedExecution().return_value;
    }

    const unsigned normal_entries =
        static_cast<unsigned>(summary.getPrePostList().size()) - latent_added;
    if (normal_entries < max_normal_entries) {
      AbductiveDomain pruned = pruneForSummary(F, *astate, ret_val);
      auto pre = std::make_unique<AbductiveDomain>(pruned.clone());
      auto post = std::make_unique<AbductiveDomain>(std::move(pruned));
      summary.addPrePost(SummaryEntry(std::move(pre), formula.clone(),
                                      std::move(post), formula.clone(),
                                      ret_val));
      continue;
    }
    // Budget exhausted: for sound incorrectness, do not merge multiple exit
    // states into a single summary entry. Union-style merging can fabricate
    // heap facts and admit non-witnessable caller paths. Prefer dropping extra
    // disjuncts (reduces recall, preserves witnessability).
    break;
  }

  if (!has_any_entry) {
    return;
  }

  // Store formal parameter mappings
  for (const auto &Arg : F->args()) {
    AbstractValue formal_av = factory_.getOrCreate(&Arg);
    summary.setFormalAV(&Arg, formal_av);
  }

  summary_manager_.storeSummary(F, std::move(summary));
}

std::vector<ExecutionDomain> PulseChecker::applySummary(
    const llvm::Function *callee, const ExecutionDomain &caller_state,
    const llvm::CallInst *CI, const llvm::BasicBlock *pred) {

  const PulseSummary *summary_ptr = summary_manager_.getSummary(callee);
  if (!summary_ptr || !summary_ptr->isValid()) {
    return {};
  }

  const PulseSummary &summary = *summary_ptr;
  std::vector<ExecutionDomain> results;

  const AbductiveDomain *caller_astate = caller_state.getAstate();
  if (!caller_astate)
    return {caller_state};

  // Create new state by applying summary
  ExecutionDomain new_state = caller_state.clone();
  auto *new_astate = new_state.getAstate();
  if (!new_astate)
    return {caller_state};

  // Build substitution: map formal parameters to actual arguments
  Substitution substitution;

  const auto *pre = summary.getPre();
  const auto *post = summary.getPost();

  unsigned arg_idx = 0;
  for (const auto &Arg : callee->args()) {
    if (arg_idx >= CI->arg_size())
      break;

    // Get actual argument value (use new_astate which is non-const)
    auto actual_opt =
        ops_.eval(*new_astate, CI->getArgOperand(arg_idx), CI, pred);
    if (actual_opt) {
      // Get formal abstract value from summary
      auto formal_av_opt = summary.getFormalAV(&Arg);
      if (formal_av_opt) {
        // Map formal to actual (canonicalize both)
        AbstractValue formal_canon = pre->getCanonical(*formal_av_opt);
        AbstractValue actual_canon = new_astate->getCanonical(actual_opt->addr);
        substitution.add(formal_canon, actual_canon);
      }
    }
    arg_idx++;
  }

  // Check for contradictions: verify pre-condition is satisfied
  // Check if actual arguments satisfy the pre-condition's constraints
  bool has_contradiction = false;
  for (const auto &kv : pre->getPreStack().getMap()) {
    auto formal_av_opt = summary.getFormalAV(kv.first);
    if (!formal_av_opt)
      continue;

    AbstractValue formal_canon = pre->getCanonical(*formal_av_opt);
    auto actual_opt = substitution.substitute(formal_canon);
    if (!actual_opt)
      continue;

    // Check if pre-condition attributes are satisfied
    const auto &pre_attrs = pre->getPreAttrs().get(*formal_av_opt);
    for (Attribute attr : pre_attrs) {
      if (attr == Attribute::Null) {
        // Pre-condition says formal is null, but actual might not be
        if (!new_astate->getPathFormula().isNull(*actual_opt) &&
            !new_astate->getPostAttrs().has(*actual_opt, Attribute::Null)) {
          // Contradiction: pre says null but actual is not null
          has_contradiction = true;
          break;
        }
      } else if (attr == Attribute::Allocated) {
        // Pre-condition says formal is allocated
        if (new_astate->getPostAttrs().has(*actual_opt, Attribute::Invalid)) {
          // Contradiction: pre says allocated but actual is invalid
          has_contradiction = true;
          break;
        }
      }
    }
    if (has_contradiction)
      break;
  }

  // Check path condition contradictions
  if (!has_contradiction) {
    // Merge caller's path condition with callee's pre-formula (after
    // substitution)
    PulseFormula caller_formula = new_astate->getPathFormula().clone();
    PulseFormula callee_pre_formula = summary.getPreFormula();

    // Apply substitution to callee's pre-formula constraints
    // For now, we do a simplified check: if caller has constraints that
    // contradict the substituted pre-formula, we have a contradiction (Full
    // implementation would substitute all values in the formula)

    // Simple check: if pre-formula has null constraints that contradict caller
    for (const auto &kv : pre->getPreStack().getMap()) {
      auto formal_av_opt = summary.getFormalAV(kv.first);
      if (!formal_av_opt)
        continue;

      AbstractValue formal_canon = pre->getCanonical(*formal_av_opt);
      auto actual_opt = substitution.substitute(formal_canon);
      if (!actual_opt)
        continue;

      // Check null/non-null contradictions
      if (callee_pre_formula.isNull(formal_canon)) {
        if (caller_formula.isNonNull(*actual_opt)) {
          has_contradiction = true;
          break;
        }
      } else if (callee_pre_formula.isNonNull(formal_canon)) {
        if (caller_formula.isNull(*actual_opt)) {
          has_contradiction = true;
          break;
        }
      }
    }
  }

  if (has_contradiction) {
    // Contradiction detected - cannot apply this summary.
    return {};
  }

  // Apply post-condition from summary with substitution
  // Copy post-heap edges (with substitution)
  for (const auto &kv : post->getPostHeap().getEdges()) {
    AbstractValue formal_from = kv.first;
    AbstractValue actual_from = substitution.substituteOrIdentity(formal_from);

    for (const auto &edge_kv : kv.second) {
      const Access &access = edge_kv.first;
      const Address &formal_target = edge_kv.second;

      // Substitute target address
      Address actual_target = applySubstitution(substitution, formal_target);

      // Add edge to caller's heap
      if (!new_astate->getPostHeap().findEdge(actual_from, access)) {
        new_astate->getPostHeap().addEdge(actual_from, access, actual_target);
      }
    }
  }

  // Copy post-attributes (with substitution)
  for (const auto &kv : post->getPostAttrs().getAttrs()) {
    AbstractValue formal_av = kv.first;
    AbstractValue actual_av = substitution.substituteOrIdentity(formal_av);

    for (Attribute attr : kv.second) {
      new_astate->getPostAttrs().add(actual_av, attr);
    }
  }

  // Copy allocation sizes (with substitution)
  for (const auto &kv : post->getAllocationSizes()) {
    AbstractValue formal_av = post->getCanonical(kv.first);
    AbstractValue actual_av = substitution.substituteOrIdentity(formal_av);
    new_astate->setAllocationSize(actual_av, kv.second);
  }

  // Merge post-formula (with substitution applied conceptually)
  PulseFormula post_formula = summary.getPostFormula();
  // Merge formulas (substitution is handled implicitly through
  // canonicalization)
  PulseFormula merged_formula =
      PulseFormula::merge(new_astate->getPathFormula(), post_formula);
  if (merged_formula.isConsistent()) {
    new_astate->setPathFormula(
        std::make_unique<PulseFormula>(std::move(merged_formula)));
  }

  // Handle return value with substitution
  if (summary.getReturnValue()) {
    AbstractValue formal_ret = *summary.getReturnValue();
    AbstractValue actual_ret = substitution.substituteOrIdentity(formal_ret);

    // If return value was substituted, use it; otherwise create fresh
    if (substitution.substitute(formal_ret)) {
      Address ret_addr(actual_ret);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      new_astate->getPostStack().add(CI, ret_addr);
    } else {
      // Return value not in substitution (fresh value from callee)
      // Check if the function returns a null constant
      const llvm::Function *callee = summary.getFunction();
      const llvm::Value *null_constant_ret = nullptr;

      // Check if any ReturnInst in the function returns a null constant
      for (const auto &BB : *callee) {
        if (auto *RI = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator())) {
          if (RI->getNumOperands() > 0) {
            const llvm::Value *ret_val = RI->getReturnValue();
            if (ret_val && ret_val->getType()->isPointerTy()) {
              if (detail::isNullPointerConstantValue(ret_val)) {
                null_constant_ret = ret_val;
                break;
              }
            }
          }
        }
      }

      // Create return value: use null constant as source if function returns
      // null
      AbstractValue fresh_ret;
      if (null_constant_ret) {
        fresh_ret = factory_.createFresh(null_constant_ret);
      } else {
        fresh_ret = factory_.createFresh(CI);
      }

      Address ret_addr(fresh_ret);
      ret_addr.history.addEvent(ValueHistory::EventKind::FunctionCall, CI,
                                CI->getFunction());
      new_astate->getPostStack().add(CI, ret_addr);

      // Copy attributes from formal return to fresh return
      const auto &ret_attrs = post->getPostAttrs().get(formal_ret);
      for (Attribute attr : ret_attrs) {
        new_astate->getPostAttrs().add(fresh_ret, attr);
      }

      // If function returns null constant, also set Null attribute and path
      // formula
      if (null_constant_ret) {
        new_astate->getPostAttrs().add(fresh_ret, Attribute::Null);
        new_astate->getPathFormula().addNull(fresh_ret);
      }
    }
  }

  results.push_back(std::move(new_state));
  return results;
}

} // namespace pulse
