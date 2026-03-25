
#include "Checker/Pulse/Domain/PulseDisjunctiveDomain.h"

#include <algorithm>

namespace pulse {

//===----------------------------------------------------------------------===//
// DisjunctiveDomain
//
// Tracks a bounded set of disjunctive states per basic block entry.
//
// Sound incorrectness guidance:
// - Prefer keeping distinct disjuncts (witness paths) rather than aggressively
//   merging them, because merges can forget the very information needed to make
//   a bug witnessable.
// - Widening/limits are scalability mechanisms; they may reduce recall. When
//   forced to merge, this implementation avoids *conjoining* path conditions
//   (which would drop feasible witnesses).
//===----------------------------------------------------------------------===//

static void reduceDisjuncts(std::vector<DisjunctiveDomain::Disjunct> &disjuncts,
                            size_t max) {
  if (disjuncts.size() <= max) {
    return;
  }

  auto is_preferred = [](const DisjunctiveDomain::Disjunct &d) {
    auto *a = d.state.getAstate();
    return a && !a->hasUnknownValues();
  };

  std::vector<DisjunctiveDomain::Disjunct> preferred;
  std::vector<DisjunctiveDomain::Disjunct> rest;
  preferred.reserve(disjuncts.size());
  rest.reserve(disjuncts.size());

  for (auto &d : disjuncts) {
    if (is_preferred(d)) {
      preferred.push_back(std::move(d));
    } else {
      rest.push_back(std::move(d));
    }
  }

  std::vector<DisjunctiveDomain::Disjunct> selected;
  selected.reserve(max);
  std::set<const llvm::BasicBlock *> seen_ctx;

  auto pick = [&](std::vector<DisjunctiveDomain::Disjunct> &pool) {
    for (auto &d : pool) {
      if (selected.size() >= max) {
        break;
      }
      if (!d.path_context) {
        continue;
      }
      if (seen_ctx.insert(d.path_context).second) {
        selected.push_back(std::move(d));
      }
    }
    for (auto &d : pool) {
      if (selected.size() >= max) {
        break;
      }
      selected.push_back(std::move(d));
    }
  };

  pick(preferred);
  pick(rest);

  disjuncts.swap(selected);
}

const std::vector<DisjunctiveDomain::Disjunct> &
DisjunctiveDomain::getDisjuncts(const llvm::BasicBlock *at_block) const {
  static const std::vector<DisjunctiveDomain::Disjunct> kEmpty;
  auto it = disjuncts_by_block_.find(at_block);
  return (it == disjuncts_by_block_.end()) ? kEmpty : it->second;
}

std::vector<DisjunctiveDomain::Disjunct> &
DisjunctiveDomain::getDisjuncts(const llvm::BasicBlock *at_block) {
  return disjuncts_by_block_[at_block];
}

size_t DisjunctiveDomain::size() const {
  size_t total = 0;
  for (const auto &kv : disjuncts_by_block_) {
    total += kv.second.size();
  }
  return total;
}

void DisjunctiveDomain::add(const llvm::BasicBlock *at_block,
                            ExecutionDomain state,
                            const llvm::BasicBlock *path_context) {
  // Preserve predecessor context for sound PHI evaluation.
  state.setEntryPred(path_context);
  disjuncts_by_block_[at_block].emplace_back(std::move(state), path_context);
  limitDisjuncts(at_block);
}

void DisjunctiveDomain::limitDisjuncts(const llvm::BasicBlock *at_block) {
  auto it = disjuncts_by_block_.find(at_block);
  if (it == disjuncts_by_block_.end()) {
    return;
  }
  reduceDisjuncts(it->second, kMaxDisjuncts);
}

ExecutionDomain DisjunctiveDomain::joinAtBlock(const llvm::BasicBlock *BB) {
  auto it = disjuncts_by_block_.find(BB);
  if (it == disjuncts_by_block_.end() || it->second.empty()) {
    return ExecutionDomain();
  }

  auto &disjuncts = it->second;
  if (disjuncts.size() == 1) {
    return disjuncts[0].state.clone();
  }

  // Sound incorrectness: avoid over-approximating joins under widening.
  // A union-style merge can fabricate heap facts and admit non-witnessable bug
  // paths (false positives). When forced to reduce disjuncts, keep a single
  // representative witness state instead.
  const DisjunctiveDomain::Disjunct *best = nullptr;
  for (const auto &disj : disjuncts) {
    if (disj.state.isStopped()) {
      continue;
    }
    if (!best) {
      best = &disj;
      continue;
    }
    auto *a_best = best->state.getAstate();
    auto *a_cur = disj.state.getAstate();
    const bool best_unknown = a_best ? a_best->hasUnknownValues() : true;
    const bool cur_unknown = a_cur ? a_cur->hasUnknownValues() : true;
    if (best_unknown && !cur_unknown) {
      best = &disj;
    }
  }
  if (best) {
    return best->state.clone();
  }

  ExecutionDomain stopped;
  stopped.setState(ExecutionState::Stopped);
  return stopped;
}

bool DisjunctiveDomain::shouldWiden(const llvm::BasicBlock *BB) const {
  auto it = block_iterations_.find(BB);
  if (it == block_iterations_.end()) {
    return false;
  }
  return it->second >= kWidenThreshold;
}

void DisjunctiveDomain::widen(const llvm::BasicBlock *BB) {
  auto it = block_iterations_.find(BB);
  if (it == block_iterations_.end()) {
    block_iterations_[BB] = 1;
  } else {
    it->second++;
  }

  // Apply widening if threshold reached
  if (block_iterations_[BB] >= kWidenThreshold) {
    auto it = disjuncts_by_block_.find(BB);
    if (it != disjuncts_by_block_.end()) {
      reduceDisjuncts(it->second, kWidenKeepDisjuncts);
    }
  }
}

} // namespace pulse
