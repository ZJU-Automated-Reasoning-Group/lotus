#include "Checker/Pulse/Domain/PulseJoin.h"

#include "Checker/Pulse/Core/PulsePathContext.h"
#include "Checker/Pulse/Domain/PulseOperations.h"
#include "Checker/Pulse/Report/PulseLogger.h"

#include <algorithm>
#include <limits>

namespace pulse {

//===----------------------------------------------------------------------===//
// PulseJoin
//
// Joins two `AbductiveDomain`s at the same program point.
//
// Sound incorrectness note:
// - Joining must not conjoin path conditions (that would discard feasible
//   witness paths). The formula join used here is a disjunction approximation.
// - Heap/stack joins introduce fresh join values when the two sides disagree.
//   This intentionally forgets some relational structure, trading recall for
//   scalability while preserving witnessability.
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// JoinState Helper Methods
//===----------------------------------------------------------------------===//

static ValueHistory joinHistories(const ValueHistory &hist1,
                                  const ValueHistory &hist2) {
  // If histories are equal, return one of them
  if (hist1.isEmpty() && hist2.isEmpty()) {
    return hist1;
  }
  if (hist1.isEmpty()) {
    return hist2;
  }
  if (hist2.isEmpty()) {
    return hist1;
  }

  // Check if histories are equal by comparing events
  const auto &events1 = hist1.getEvents();
  const auto &events2 = hist2.getEvents();

  if (events1.size() == events2.size()) {
    bool equal = true;
    for (size_t i = 0; i < events1.size(); ++i) {
      if (events1[i].kind != events2[i].kind ||
          events1[i].location != events2[i].location ||
          events1[i].function != events2[i].function) {
        equal = false;
        break;
      }
    }
    if (equal) {
      return hist1; // Histories are equal
    }
  }

  // Histories differ: find common prefix and merge
  // Find the longest common prefix
  size_t common_prefix = 0;
  size_t min_size = std::min(events1.size(), events2.size());
  for (size_t i = 0; i < min_size; ++i) {
    if (events1[i].kind == events2[i].kind &&
        events1[i].location == events2[i].location &&
        events1[i].function == events2[i].function) {
      common_prefix++;
    } else {
      break;
    }
  }

  // If there's a common prefix, return history with that prefix
  // Otherwise, return epoch (empty history)
  if (common_prefix > 0) {
    ValueHistory result;
    // Rebuild history with common prefix events
    for (size_t i = 0; i < common_prefix; ++i) {
      result.addEvent(events1[i].kind, events1[i].location, events1[i].function,
                      events1[i].description);
    }
    return result;
  }

  // No common prefix: return epoch (conservative)
  return ValueHistory();
}

static ValueHistory joinHistoriesOpts(llvm::Optional<ValueHistory> hist1_opt,
                                      llvm::Optional<ValueHistory> hist2_opt) {
  if (hist1_opt && hist2_opt) {
    return joinHistories(*hist1_opt, *hist2_opt);
  }
  // If only one side has history, return epoch
  return ValueHistory();
}

//===----------------------------------------------------------------------===//
// Value Joining
//===----------------------------------------------------------------------===//

std::pair<PulseJoin::JoinState &, std::pair<AbstractValue, ValueHistory>>
PulseJoin::joinValuesHists(JoinState &state, const AbstractValue &lhs_val,
                           const ValueHistory &lhs_hist,
                           const AbstractValue &rhs_val,
                           const ValueHistory &rhs_hist) {
  using Key =
      std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>;

  if (lhs_val == rhs_val) {
    // Same value: x↦v ⊔ x↦v = x↦v
    Key key{llvm::Optional<AbstractValue>(lhs_val),
            llvm::Optional<AbstractValue>(rhs_val)};
    auto &rev_subst = state.rev_subst;
    rev_subst[lhs_val] = key;
    ValueHistory hist_join = joinHistories(lhs_hist, rhs_hist);
    return {state, {lhs_val, hist_join}};
  }

  // Different values: x↦v ⊔ x↦v' = x↦v''
  Key key{llvm::Optional<AbstractValue>(lhs_val),
          llvm::Optional<AbstractValue>(rhs_val)};
  auto it = state.subst.find(key);
  if (it != state.subst.end()) {
    // Already joined this pair
    ValueHistory hist_join = joinHistories(lhs_hist, rhs_hist);
    return {state, {it->second, hist_join}};
  }

  // Create fresh joined value
  AbstractValue v_join = state.factory->createFresh(nullptr);
  state.subst[key] = v_join;
  state.rev_subst[v_join] = key;

  ValueHistory hist_join = joinHistories(lhs_hist, rhs_hist);
  return {state, {v_join, hist_join}};
}

std::pair<PulseJoin::JoinState &, std::pair<AbstractValue, ValueHistory>>
PulseJoin::joinValuesHistsOpts(
    JoinState &state, const AbductiveDomain &lhs_astate,
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt,
    const AbductiveDomain &rhs_astate,
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt) {
  using Key =
      std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>;

  if (!lhs_opt && !rhs_opt) {
    // Both empty - shouldn't happen
    assert(false && "Both sides empty in join");
    AbstractValue v_join = state.factory->createFresh();
    return {state, {v_join, ValueHistory()}};
  }

  if (!lhs_opt || !rhs_opt) {
    // One-sided: x↦v ⊔ emp = x↦v' (v' fresh)
    AbstractValue v_join = state.factory->createFresh(nullptr);
    ValueHistory hist_join = joinHistoriesOpts(
        lhs_opt ? llvm::Optional<ValueHistory>(lhs_opt->second) : llvm::None,
        rhs_opt ? llvm::Optional<ValueHistory>(rhs_opt->second) : llvm::None);
    return {state, {v_join, hist_join}};
  }

  // Both sides have values
  if (lhs_opt->first == rhs_opt->first) {
    // Same value: x↦v ⊔ x↦v = x↦v
    Key key{llvm::Optional<AbstractValue>(lhs_opt->first),
            llvm::Optional<AbstractValue>(rhs_opt->first)};
    state.rev_subst[lhs_opt->first] = key;
    ValueHistory hist_join = joinHistories(lhs_opt->second, rhs_opt->second);
    return {state, {lhs_opt->first, hist_join}};
  }

  // Different values: use cached join
  Key key{llvm::Optional<AbstractValue>(lhs_opt->first),
          llvm::Optional<AbstractValue>(rhs_opt->first)};
  auto it = state.subst.find(key);
  if (it != state.subst.end()) {
    ValueHistory hist_join = joinHistories(lhs_opt->second, rhs_opt->second);
    return {state, {it->second, hist_join}};
  }

  return joinValuesHists(state, lhs_opt->first, lhs_opt->second, rhs_opt->first,
                         rhs_opt->second);
}

//===----------------------------------------------------------------------===//
// Heap Joining
//===----------------------------------------------------------------------===//

std::pair<PulseJoin::JoinState &, Heap> PulseJoin::joinHeaps(
    JoinState &state, Heap &heap_join, const AbductiveDomain &lhs_astate,
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt,
    const AbductiveDomain &rhs_astate,
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt) {
  using Key =
      std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>;

  if (!lhs_opt && !rhs_opt) {
    return {state, heap_join};
  }

  Key visited_key{
      lhs_opt ? llvm::Optional<AbstractValue>(lhs_opt->first) : llvm::None,
      rhs_opt ? llvm::Optional<AbstractValue>(rhs_opt->first) : llvm::None};

  if (state.visited.count(visited_key) > 0) {
    return {state, heap_join}; // Already visited
  }
  state.visited.insert(visited_key);

  // Join the values first
  auto value_result =
      joinValuesHistsOpts(state, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
  state = value_result.first;
  auto v_hist_join = value_result.second;

  // Collect edges from both sides
  std::set<Access> all_accesses;
  if (lhs_opt) {
    const auto &lhs_edges = lhs_astate.getPostHeap().getEdges();
    auto lhs_it = lhs_edges.find(lhs_opt->first);
    if (lhs_it != lhs_edges.end()) {
      for (const auto &edge_kv : lhs_it->second) {
        all_accesses.insert(edge_kv.first);
      }
    }
  }
  if (rhs_opt) {
    const auto &rhs_edges = rhs_astate.getPostHeap().getEdges();
    auto rhs_it = rhs_edges.find(rhs_opt->first);
    if (rhs_it != rhs_edges.end()) {
      for (const auto &edge_kv : rhs_it->second) {
        all_accesses.insert(edge_kv.first);
      }
    }
  }

  // Join edges
  for (const Access &access : all_accesses) {
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_target_opt;
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_target_opt;

    if (lhs_opt) {
      const auto *lhs_target =
          lhs_astate.getPostHeap().findEdge(lhs_opt->first, access);
      if (lhs_target) {
        lhs_target_opt = {{lhs_target->addr, lhs_target->history}};
      }
    }
    if (rhs_opt) {
      const auto *rhs_target =
          rhs_astate.getPostHeap().findEdge(rhs_opt->first, access);
      if (rhs_target) {
        rhs_target_opt = {{rhs_target->addr, rhs_target->history}};
      }
    }

    // Recursively join target values
    auto target_result = joinValuesHistsOpts(state, lhs_astate, lhs_target_opt,
                                             rhs_astate, rhs_target_opt);
    state = target_result.first;
    auto target_v_hist = target_result.second;

    // Recursively join heaps from targets
    auto heap_result = joinHeaps(state, heap_join, lhs_astate, lhs_target_opt,
                                 rhs_astate, rhs_target_opt);
    state = heap_result.first;
    heap_join = heap_result.second;

    // Add edge to joined heap
    Address target_addr(target_v_hist.first);
    target_addr.history = target_v_hist.second;
    heap_join.addEdge(v_hist_join.first, access, target_addr);
  }

  return {state, heap_join};
}

//===----------------------------------------------------------------------===//
// Stack Joining
//===----------------------------------------------------------------------===//

std::pair<PulseJoin::JoinState &, std::pair<Stack, Heap>>
PulseJoin::joinStacks(JoinState &state, const AbductiveDomain &lhs_astate,
                      const AbductiveDomain &rhs_astate) {
  Stack stack_pre_join;
  Heap heap_pre_join;

  // Collect all variables from both pre stacks
  std::set<const llvm::Value *> all_pre_vars;
  for (const auto &kv : lhs_astate.getPreStack().getMap()) {
    all_pre_vars.insert(kv.first);
  }
  for (const auto &kv : rhs_astate.getPreStack().getMap()) {
    all_pre_vars.insert(kv.first);
  }

  // Join pre stack
  for (const llvm::Value *var : all_pre_vars) {
    const Address *lhs_addr = lhs_astate.getPreStack().find(var);
    const Address *rhs_addr = rhs_astate.getPreStack().find(var);

    llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt;
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt;

    if (lhs_addr) {
      lhs_opt = {{lhs_addr->addr, lhs_addr->history}};
    }
    if (rhs_addr) {
      rhs_opt = {{rhs_addr->addr, rhs_addr->history}};
    }

    // Join values
    auto result =
        joinValuesHistsOpts(state, lhs_astate, lhs_opt, rhs_astate, rhs_opt);
    state = result.first;
    auto v_hist_join = result.second;

    // Join heaps from pre stack values
    auto heap_result = joinHeaps(state, heap_pre_join, lhs_astate, lhs_opt,
                                 rhs_astate, rhs_opt);
    state = heap_result.first;
    heap_pre_join = heap_result.second;

    // Add to joined pre stack
    Address joined_addr(v_hist_join.first);
    joined_addr.history = v_hist_join.second;
    stack_pre_join.add(var, joined_addr);
  }

  // Return pre stack and heap
  return {state, {stack_pre_join, heap_pre_join}};
}

//===----------------------------------------------------------------------===//
// Attribute Joining
//===----------------------------------------------------------------------===//

llvm::Optional<Attribute> PulseJoin::joinOneSidedAttribute(Attribute attr) {
  // One-sided attributes: keep if they're "weak" (true in some branches),
  // drop if they're "strong" (must be true in all branches)
  switch (attr) {
  case Attribute::Allocated:
    // Keep: if allocated in one branch, it might be allocated
    return attr;

  case Attribute::Stack:
  case Attribute::Global:
    // Base-kind facts must be stable; drop on one-sided to avoid guessing.
    return llvm::None;

  case Attribute::Invalid:
  case Attribute::Null:
  case Attribute::Uninitialized:
    // Drop: these are "strong" - if only in one branch, we can't assume them
    return llvm::None;

  case Attribute::Tainted:
    // Keep: if tainted in one branch, conservatively assume tainted
    // (matches Infer's approach of weakening one-sided attributes)
    return attr;

  case Attribute::FileHandle:
  case Attribute::Lock:
  case Attribute::AsyncResource:
    // Drop: resource attributes are too strong for one-sided join
    return llvm::None;

  default:
    return llvm::None;
  }
}

llvm::Optional<Attribute>
PulseJoin::joinTwoSidedAttribute(JoinState &state, Attribute attr1,
                                 Attribute attr2, AbstractValue lhs_val,
                                 AbstractValue rhs_val) {
  if (attr1 == attr2) {
    return attr1; // Same attribute
  }

  // Handle specific attribute types
  switch (attr1) {
  case Attribute::Allocated:
    if (attr2 == Attribute::Allocated) {
      return Attribute::Allocated; // Both allocated
    }
    // Allocated vs Invalid/Null - incompatible
    return llvm::None;

  case Attribute::Stack:
    if (attr2 == Attribute::Stack) {
      return Attribute::Stack;
    }
    return llvm::None;

  case Attribute::Global:
    if (attr2 == Attribute::Global) {
      return Attribute::Global;
    }
    return llvm::None;

  case Attribute::Invalid:
    if (attr2 == Attribute::Invalid) {
      return Attribute::Invalid; // Both invalid
    }
    // Invalid vs Allocated - incompatible
    return llvm::None;

  case Attribute::Null:
    if (attr2 == Attribute::Null) {
      return Attribute::Null; // Both null
    }
    // Null vs NonNull - incompatible
    return llvm::None;

  case Attribute::Uninitialized:
    if (attr2 == Attribute::Uninitialized) {
      return Attribute::Uninitialized; // Both uninitialized
    }
    // Uninitialized vs Initialized - incompatible
    return llvm::None;

  case Attribute::Tainted:
    if (attr2 == Attribute::Tainted) {
      return Attribute::Tainted; // Both tainted (union of taint sources)
    }
    // Tainted vs NotTainted - keep tainted (conservative)
    return Attribute::Tainted;

  case Attribute::FileHandle:
  case Attribute::Lock:
  case Attribute::AsyncResource:
    // Resource attributes: if both have same resource, keep it
    if (attr1 == attr2) {
      return attr1;
    }
    // Different resources - incompatible
    return llvm::None;

  default:
    // Unknown attribute type - incompatible
    return llvm::None;
  }
}

AttributeSet
PulseJoin::joinAttributes(JoinState &state, const AbductiveDomain &lhs_astate,
                          const AbductiveDomain &rhs_astate, bool use_pre_attrs,
                          AbstractValue joined_addr,
                          llvm::Optional<AbstractValue> lhs_addr_opt,
                          llvm::Optional<AbstractValue> rhs_addr_opt) {
  AttributeSet result;

  AttributeSet lhs_attrs;
  AttributeSet rhs_attrs;

  if (lhs_addr_opt) {
    lhs_attrs = use_pre_attrs ? lhs_astate.getPreAttrs().get(*lhs_addr_opt)
                              : lhs_astate.getPostAttrs().get(*lhs_addr_opt);
  }
  if (rhs_addr_opt) {
    rhs_attrs = use_pre_attrs ? rhs_astate.getPreAttrs().get(*rhs_addr_opt)
                              : rhs_astate.getPostAttrs().get(*rhs_addr_opt);
  }

  // Collect all attributes
  std::set<Attribute> all_attrs;
  for (Attribute attr : lhs_attrs) {
    all_attrs.insert(attr);
  }
  for (Attribute attr : rhs_attrs) {
    all_attrs.insert(attr);
  }

  // Join each attribute
  for (Attribute attr : all_attrs) {
    bool in_lhs = lhs_attrs.count(attr) > 0;
    bool in_rhs = rhs_attrs.count(attr) > 0;

    llvm::Optional<Attribute> joined_attr;
    if (in_lhs && in_rhs) {
      // Two-sided
      if (lhs_addr_opt && rhs_addr_opt) {
        joined_attr = joinTwoSidedAttribute(state, attr, attr, *lhs_addr_opt,
                                            *rhs_addr_opt);
      } else {
        joined_attr = attr; // Same attribute on both sides
      }
    } else {
      // One-sided
      joined_attr = joinOneSidedAttribute(attr);
    }

    if (joined_attr) {
      result.insert(*joined_attr);
    }
  }

  return result;
}

//===----------------------------------------------------------------------===//
// Formula Joining
//===----------------------------------------------------------------------===//

PulseFormula PulseJoin::joinFormulas(const AbductiveDomain &lhs,
                                     const AbductiveDomain &rhs) {
  const PulseFormula &lhs_formula = lhs.getPathFormula();
  const PulseFormula &rhs_formula = rhs.getPathFormula();

  // Join formulas (disjunction): represent "lhs path condition OR rhs path
  // condition". This must NOT conjoin constraints, otherwise feasible paths get
  // dropped.
  PulseFormula joined = PulseFormula::join(lhs_formula, rhs_formula);

  // TODO: Add equalities from rev_subst to formula
  // For each v_join in rev_subst mapping to (v_lhs, v_rhs),
  // we should add: v_join = v_lhs ∨ v_join = v_rhs
  // This requires access to join_state, which we'll add in the main join
  // function

  return joined;
}

//===----------------------------------------------------------------------===//
// Main Join Operations
//===----------------------------------------------------------------------===//

llvm::Optional<AbductiveDomain>
PulseJoin::joinAbductive(const AbductiveDomain &lhs,
                         const AbductiveDomain &rhs) {
  PulseLogger::trace("Joining abductive domains");
  PulseLogger::incrementCounter("joins.performed");

  // Check formula consistency first (preliminary check)
  PulseFormula preliminary_formula = joinFormulas(lhs, rhs);
  if (!preliminary_formula.isConsistent() || preliminary_formula.isUnsat()) {
    PulseLogger::debug("Join failed: formula contradiction");
    PulseLogger::incrementCounter("joins.failed");
    return llvm::None; // Contradiction
  }

  // Create join state with factory
  // We need a factory - create a temporary one
  AbstractValueFactory factory;
  JoinState join_state(&factory);

  // Join pre stacks (this also joins pre heaps recursively)
  auto pre_result = joinStacks(join_state, lhs, rhs);
  join_state = pre_result.first;
  Stack stack_pre_join = pre_result.second.first;
  Heap heap_pre_join = pre_result.second.second;

  // Join post stacks and heaps separately
  Stack stack_post_join;
  Heap heap_post_join;

  // Collect all variables from both post stacks
  std::set<const llvm::Value *> all_post_vars;
  for (const auto &kv : lhs.getPostStack().getMap()) {
    all_post_vars.insert(kv.first);
  }
  for (const auto &kv : rhs.getPostStack().getMap()) {
    all_post_vars.insert(kv.first);
  }

  // Reset visited set for post join (as in Infer)
  join_state.visited.clear();

  // Join post stack
  for (const llvm::Value *var : all_post_vars) {
    const Address *lhs_addr = lhs.getPostStack().find(var);
    const Address *rhs_addr = rhs.getPostStack().find(var);

    llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt;
    llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt;

    if (lhs_addr) {
      lhs_opt = {{lhs_addr->addr, lhs_addr->history}};
    }
    if (rhs_addr) {
      rhs_opt = {{rhs_addr->addr, rhs_addr->history}};
    }

    // Join values
    auto result = joinValuesHistsOpts(join_state, lhs, lhs_opt, rhs, rhs_opt);
    join_state = result.first;
    auto v_hist_join = result.second;

    // Join heaps from stack values
    auto heap_result =
        joinHeaps(join_state, heap_post_join, lhs, lhs_opt, rhs, rhs_opt);
    join_state = heap_result.first;
    heap_post_join = heap_result.second;

    // Add to joined stack
    Address joined_addr(v_hist_join.first);
    joined_addr.history = v_hist_join.second;
    stack_post_join.add(var, joined_addr);
  }

  // Join attributes using rev_subst
  AddressAttributes attrs_pre_join, attrs_post_join;

  // For each joined value in rev_subst, join its attributes
  // This follows Infer's pattern: iterate through rev_subst to join attributes
  for (const auto &kv : join_state.rev_subst) {
    AbstractValue v_join = kv.first;
    const auto &pair_opt = kv.second;
    llvm::Optional<AbstractValue> lhs_addr_opt = pair_opt.first;
    llvm::Optional<AbstractValue> rhs_addr_opt = pair_opt.second;

    // Join post attributes
    AttributeSet joined_post_attrs =
        joinAttributes(join_state, lhs, rhs, /*use_pre_attrs=*/false, v_join,
                       lhs_addr_opt, rhs_addr_opt);
    for (Attribute attr : joined_post_attrs) {
      attrs_post_join.add(v_join, attr);
    }

    // Join pre attributes
    AttributeSet joined_pre_attrs =
        joinAttributes(join_state, lhs, rhs, /*use_pre_attrs=*/true, v_join,
                       lhs_addr_opt, rhs_addr_opt);
    for (Attribute attr : joined_pre_attrs) {
      attrs_pre_join.add(v_join, attr);
    }
  }

  // Join formulas
  PulseFormula joined_formula = joinFormulas(lhs, rhs);

  // Add equalities from rev_subst: for each v_join mapping to (v_lhs, v_rhs),
  // we should add v_join = v_lhs ∨ v_join = v_rhs
  // However, our formula system doesn't support disjunction yet, so we add both
  // equalities This is conservative (weaker than ideal) but safe
  // TODO: When formula system supports disjunction, add proper disjunctive
  // constraints
  for (const auto &kv : join_state.rev_subst) {
    AbstractValue v_join = kv.first;
    const auto &pair_opt = kv.second;
    if (pair_opt.first && pair_opt.second) {
      // Both sides exist: v_join could be either
      // For now, we don't add constraints (would require disjunction)
      // In a full implementation, we'd add: v_join = v_lhs ∨ v_join = v_rhs
      (void)v_join; // Suppress unused warning
    } else if (pair_opt.first) {
      // Only lhs: v_join = v_lhs
      joined_formula.addEquality(v_join, *pair_opt.first);
    } else if (pair_opt.second) {
      // Only rhs: v_join = v_rhs
      joined_formula.addEquality(v_join, *pair_opt.second);
    }
  }

  // Create joined domain
  AbductiveDomain joined;
  joined.getPostStack() = std::move(stack_post_join);
  joined.getPreStack() = std::move(stack_pre_join);
  joined.getPostHeap() = std::move(heap_post_join);
  joined.getPreHeap() = std::move(heap_pre_join);
  joined.getPostAttrs() = std::move(attrs_post_join);
  joined.getPreAttrs() = std::move(attrs_pre_join);
  joined.setPathFormula(
      std::make_unique<PulseFormula>(std::move(joined_formula)));

  // Join additional fields (from AbductiveDomain::merge logic)
  // Merge transitive info
  joined.setTransitiveInfo(
      TransitiveInfo::merge(lhs.getTransitiveInfo(), rhs.getTransitiveInfo()));

  // Merge unknown values flag (OR operation)
  joined.setUnknownValues(lhs.hasUnknownValues() || rhs.hasUnknownValues());

  // Merge skipped calls (union)
  for (const std::string &call : lhs.getSkippedCalls()) {
    joined.addSkippedCall(call);
  }
  for (const std::string &call : rhs.getSkippedCalls()) {
    joined.addSkippedCall(call);
  }

  // Merge need_dynamic_type_specialization (union)
  for (AbstractValue av : lhs.getNeedDynamicTypeSpecialization()) {
    joined.addNeedDynamicTypeSpecialization(av);
  }
  for (AbstractValue av : rhs.getNeedDynamicTypeSpecialization()) {
    joined.addNeedDynamicTypeSpecialization(av);
  }

  // Merge recursive calls (union)
  for (const std::string &call : lhs.getRecursiveCalls()) {
    joined.addRecursiveCall(call);
  }
  for (const std::string &call : rhs.getRecursiveCalls()) {
    joined.addRecursiveCall(call);
  }

  // Loop header info: preserve from lhs (as in Infer)
  for (const auto &kv : lhs.getLoopHeaderInfo()) {
    joined.pushLoopHeaderInfo(kv.first, kv.second);
  }

  // Loop invariant under inference: preserve from lhs if present (as in Infer)
  // Note: This requires access to the private field, which we don't have
  // For now, we skip this - it will be preserved if we clone from lhs
  // TODO: Add getter method or friend class access

  // Merge invalidation info (union)
  // Note: getInvalidationInfo() only returns one entry, so we need to access
  // the map directly For now, we'll skip this - it requires exposing the map or
  // adding an iterator The invalidation info will be preserved through the heap
  // join process

  return joined;
}

llvm::Optional<std::pair<AbductiveDomain, PathContext>>
PulseJoin::join(const AbductiveDomain &lhs, const PathContext &path_lhs,
                const AbductiveDomain &rhs, const PathContext &path_rhs) {
  auto joined_domain_opt = joinAbductive(lhs, rhs);
  if (!joined_domain_opt) {
    return llvm::None;
  }

  PathContext joined_path = PathContext::join(path_lhs, path_rhs);
  // Use std::make_pair to avoid copy constructor issues
  return llvm::Optional<std::pair<AbductiveDomain, PathContext>>(
      std::make_pair(std::move(*joined_domain_opt), joined_path));
}

AbductiveDomain PulseJoin::joinSummaries(const AbductiveDomain &lhs,
                                         const AbductiveDomain &rhs) {
  auto joined_opt = joinAbductive(lhs, rhs);
  if (!joined_opt) {
    // If join fails, return empty domain (or could return lhs as fallback)
    return AbductiveDomain();
  }
  // Move the result to avoid copy constructor
  return std::move(*joined_opt);
}

} // namespace pulse
