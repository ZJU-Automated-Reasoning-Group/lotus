/// @file StrongUpdates.cpp
/// @brief Tuna-style staged strong updates for LotusAA load-store matching

#include "Alias/InclusionBased/LotusAA/Engine/IntraProceduralAnalysis.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;

namespace {

static void appendInstructionSuccessors(
    const Instruction *instruction,
    SmallVectorImpl<const Instruction *> &successors_out) {
  if (const Instruction *next = instruction->getNextNode()) {
    successors_out.push_back(next);
    return;
  }

  const BasicBlock *block = instruction->getParent();
  if (!block)
    return;
  for (const BasicBlock *successor : successors(block)) {
    if (successor && !successor->empty())
      successors_out.push_back(&successor->front());
  }
}

} // namespace

bool IntraLotusAA::instructionReaches(const Instruction *source,
                                      const Instruction *target,
                                      const Instruction *avoiding) {
  if (!source || !target || source == target ||
      source->getFunction() != target->getFunction()) {
    return false;
  }

  if (!avoiding) {
    auto key = std::make_pair(source, target);
    auto cached = reachability_cache.find(key);
    if (cached != reachability_cache.end())
      return cached->second;
  } else {
    auto key = std::make_tuple(source, target, avoiding);
    auto cached = avoiding_reachability_cache.find(key);
    if (cached != avoiding_reachability_cache.end())
      return cached->second;
  }

  SmallVector<const Instruction *, 32> worklist;
  appendInstructionSuccessors(source, worklist);
  SmallPtrSet<const Instruction *, 32> visited;
  bool reaches = false;
  while (!worklist.empty()) {
    const Instruction *current = worklist.pop_back_val();
    if (!current || current == avoiding || !visited.insert(current).second)
      continue;
    if (current == target) {
      reaches = true;
      break;
    }
    appendInstructionSuccessors(current, worklist);
  }

  if (!avoiding)
    reachability_cache[std::make_pair(source, target)] = reaches;
  else
    avoiding_reachability_cache[std::make_tuple(source, target, avoiding)] =
        reaches;
  return reaches;
}

LoadInst *IntraLotusAA::findImmediateMustAliasAnchor(LoadInst *load) {
  if (!load || !dom_tree)
    return nullptr;

  LoadInst *best = nullptr;
  for (Instruction &instruction : instructions(*analyzed_func)) {
    auto *candidate = dyn_cast<LoadInst>(&instruction);
    if (!candidate || candidate == load ||
        !dom_tree->dominates(candidate, load) ||
        !areMustAliases(candidate->getPointerOperand(),
                        load->getPointerOperand())) {
      continue;
    }

    if (!best) {
      best = candidate;
      continue;
    }

    if (best->getParent() == candidate->getParent()) {
      if (best->comesBefore(candidate))
        best = candidate;
      continue;
    }
    if (dom_tree->dominates(best->getParent(), candidate->getParent()))
      best = candidate;
  }
  return best;
}

bool IntraLotusAA::mustKill(StoreInst *killer, StoreInst *killed,
                            LoadInst *load) {
  if (!killer || !killed || !load || killer == killed)
    return false;
  if (!areMustAliases(killer->getPointerOperand(),
                      killed->getPointerOperand())) {
    return false;
  }
  if (!instructionReaches(killed, killer) ||
      !instructionReaches(killer, load)) {
    return false;
  }

  // Standard dominance discharges the common case in constant time.  The
  // reachability-with-removal fallback also catches the paper's more general
  // relation where paths that never execute the killed store may bypass the
  // killer on their way to the load.
  if (dom_tree && dom_tree->dominates(killer, load))
    return true;

  // Definition 3.1: every killed-to-load path must pass through the killer.
  // Testing reachability with the killer removed handles same-block ordering,
  // branches, and joins uniformly without requiring a full post-dominator
  // tree rooted at each load.
  return !instructionReaches(killed, load, killer);
}

IntraLotusAA::MustKillForest &
IntraLotusAA::getOrCreateMustKillForest(LoadInst *load) {
  auto cached = must_kill_forests.find(load);
  if (cached != must_kill_forests.end())
    return cached->second;

  MustKillForest forest;
  forest.anchor = findImmediateMustAliasAnchor(load);
  if (forest.anchor) {
    MustKillForest &anchor_forest = getOrCreateMustKillForest(forest.anchor);
    forest.stores = anchor_forest.stores;
    forest.roots = anchor_forest.roots;
  }

  const int load_sequence = getSequenceNum(load);
  const int anchor_sequence = forest.anchor ? getSequenceNum(forest.anchor)
                                             : VALUE_SEQ_UNDEF;
  for (Instruction &instruction : instructions(*analyzed_func)) {
    auto *store = dyn_cast<StoreInst>(&instruction);
    if (!store)
      continue;

    const int store_sequence = getSequenceNum(store);
    // LotusAA intentionally ignores cyclic CFG regions in its topological
    // transfer pass.  Do not let an unanalysed back-edge store enter a forest.
    if (store_sequence == VALUE_SEQ_UNDEF ||
        load_sequence == VALUE_SEQ_UNDEF || store_sequence >= load_sequence ||
        (forest.anchor && store_sequence <= anchor_sequence)) {
      continue;
    }
    if (forest.anchor && !instructionReaches(forest.anchor, store))
      continue;
    if (!instructionReaches(store, load))
      continue;

    path_cond_t alias_condition = getAliasCondition(
        load->getPointerOperand(), store->getPointerOperand());
    if (!isSatisfiable(alias_condition))
      continue;

    forest.stores.insert(store);
    forest.roots.insert(store);
  }

  // constructKillForest(new stores) followed by merge(anchor forest).  It is
  // sufficient to compare new stores with the surviving roots: descendants
  // killed at the anchor cannot become live again below a dominating anchor.
  std::set<Instruction *, llvm_cmp> killed_roots;
  for (Instruction *killer_instruction : forest.roots) {
    auto *killer = cast<StoreInst>(killer_instruction);
    for (Instruction *killed_instruction : forest.roots) {
      auto *killed = cast<StoreInst>(killed_instruction);
      if (mustKill(killer, killed, load))
        killed_roots.insert(killed);
    }
  }
  for (Instruction *killed : killed_roots)
    forest.roots.erase(killed);

  return must_kill_forests.emplace(load, std::move(forest)).first->second;
}

void IntraLotusAA::collectPathSensitiveLoadValues(LoadInst *load,
                                                  mem_value_t &result,
                                                  bool create_symbol) {
  result.clear();
  if (!load)
    return;

  const std::set<Instruction *, llvm_cmp> *surviving_stores = nullptr;
  if (IntraLotusAAConfig::lotus_enable_must_kill)
    surviving_stores = &getOrCreateMustKillForest(load).roots;

  if (create_symbol) {
    loadPtrAt(load->getPointerOperand(), load, result, true, 0,
              ObjectLocator::FUNC_LEVEL_UNDEFINED, nullptr, false, true,
              surviving_stores);
  } else {
    loadPtrAt(load->getPointerOperand(), load, result, false, 0, 0,
              func_obj ? func_obj->findLocator(0, false) : nullptr, true, true,
              surviving_stores);
  }
  // The GVFG adapter historically requested a refined producer set, whereas
  // transfer functions let PTResultIterator perform the final merge. Preserve
  // that distinction while sharing the must-kill filtering stage.
  if (!create_symbol)
    refineResult(result);
}

IntraLotusAA::StrongUpdateStats
IntraLotusAA::getStrongUpdateStats(LoadInst *load) {
  StrongUpdateStats stats;
  if (!load)
    return stats;
  MustKillForest &forest = getOrCreateMustKillForest(load);
  stats.anchor = forest.anchor;
  stats.candidate_store_count = static_cast<unsigned>(forest.stores.size());
  stats.root_store_count = static_cast<unsigned>(forest.roots.size());
  return stats;
}
