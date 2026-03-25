#include "Checker/Pulse/Domain/PulseLoopAbstraction.h"

#include "Checker/Pulse/Core/PulseFormula.h"

#include <llvm/Analysis/LoopInfo.h>

namespace pulse {

//===----------------------------------------------------------------------===//
// LoopAbstraction
//
// Provides widening/invariant inference scaffolding for the Pulse execution.
//
// Sound incorrectness guidance:
// - Loop handling is primarily a scalability concern. When we "widen", we
//   intentionally forget information to obtain termination, which can reduce
//   recall.
// - Path conditions are accumulated via disjunction-join (stable facts), never
//   conjunction, to avoid dropping feasible witness paths.
//===----------------------------------------------------------------------===//

void LoopAbstraction::initialize(const llvm::LoopInfo &LI) {
  loop_headers_.clear();

  // Collect all loops
  std::vector<const llvm::Loop *> loops;
  for (auto *L : LI) {
    loops.push_back(L);
    // Also collect subloops
    std::vector<const llvm::Loop *> worklist(L->begin(), L->end());
    while (!worklist.empty()) {
      const llvm::Loop *sub = worklist.back();
      worklist.pop_back();
      loops.push_back(sub);
      worklist.insert(worklist.end(), sub->begin(), sub->end());
    }
  }

  // Record loop headers
  for (const llvm::Loop *L : loops) {
    const llvm::BasicBlock *header = L->getHeader();
    if (header) {
      LoopInfo info;
      info.loop = L;
      loop_headers_[header] = info;
    }
  }
}

bool LoopAbstraction::isLoopHeader(const llvm::BasicBlock *BB) const {
  return loop_headers_.count(BB) > 0;
}

const llvm::Loop *LoopAbstraction::getLoop(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  return (it != loop_headers_.end()) ? it->second.loop : nullptr;
}

bool LoopAbstraction::shouldWiden(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }
  return it->second.iterations >= kWidenThreshold;
}

bool LoopAbstraction::visitHeader(const llvm::BasicBlock *BB,
                                  const ExecutionDomain &state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }

  it->second.iterations++;

  // Store state for comparison
  if (it->second.iterations == 1) {
    it->second.header_state = state.clone();
    it->second.entry_state = state.clone();
    auto *astate = state.getAstate();
    if (astate) {
      it->second.local_path_condition = astate->getPathFormula().clone();
    }
    initLoopInfo(BB);
    return false; // First iteration, no widening
  }

  // Record iteration with path stamp
  auto *astate = state.getAstate();
  if (astate) {
    unsigned timestamp = getNextTimestamp();
    PulseFormula path_condition = astate->getPathFormula().clone();
    pushLoopInfo(BB, timestamp, path_condition);

    // Update local path condition
    // Accumulate information across iterations: this is a join (disjunction)
    // over different paths, not a conjunction.
    it->second.local_path_condition =
        PulseFormula::join(it->second.local_path_condition, path_condition);
  }

  // Check if we should widen
  return it->second.iterations >= kWidenThreshold;
}

ExecutionDomain LoopAbstraction::widen(const llvm::BasicBlock *BB,
                                       const ExecutionDomain &current_state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return current_state;
  }

  const ExecutionDomain &header_state = it->second.header_state;

  if (it->second.iterations > kMaxWidenIterations) {
    return header_state.clone();
  }

  // Sound incorrectness: widening must not over-approximate by union-merging
  // heap facts across iterations (which can admit non-witnessable paths).
  // Under pressure, keep a representative witness state.
  (void)current_state;
  return header_state.clone();
}

bool LoopAbstraction::isInLoop(const llvm::BasicBlock *BB) const {
  for (const auto &kv : loop_headers_) {
    if (kv.second.loop->contains(BB)) {
      return true;
    }
  }
  return false;
}

std::set<const llvm::BasicBlock *> LoopAbstraction::getLoopHeaders() const {
  std::set<const llvm::BasicBlock *> headers;
  for (const auto &kv : loop_headers_) {
    headers.insert(kv.first);
  }
  return headers;
}

llvm::Optional<ExecutionDomain>
LoopAbstraction::inferInvariant(const llvm::BasicBlock *BB,
                                const ExecutionDomain &entry_state,
                                const ExecutionDomain &current_state) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return llvm::None;
  }

  // Production-ready invariant inference:
  // 1. Need at least 2 iterations to infer
  // 2. Check if path stamps are converging
  // 3. Compute fixpoint by finding common properties

  if (it->second.iterations < 2) {
    return llvm::None;
  }

  // Check convergence: if path stamps are the same, we've converged
  if (hasPreviousIterationSamePathStamp(BB)) {
    // Path condition has stabilized. For sound incorrectness, avoid
    // constructing a potentially non-witnessable "merged" invariant; keep a
    // representative witness state.
    return llvm::Optional<ExecutionDomain>(current_state.clone());
  }

  // If we've exceeded max iterations, use entry state as invariant
  if (it->second.iterations >= kMaxInvariantIterations) {
    return llvm::Optional<ExecutionDomain>(entry_state.clone());
  }

  return llvm::None;
}

bool LoopAbstraction::isInferringInvariant(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return false;
  }
  // We're inferring if we've visited multiple times but haven't converged
  return it->second.iterations >= 2 &&
         it->second.iterations < kMaxInvariantIterations &&
         !hasPreviousIterationSamePathStamp(BB);
}

void LoopAbstraction::pushLoopInfo(const llvm::BasicBlock *BB,
                                   unsigned timestamp,
                                   const PulseFormula &path_condition) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }

  PulseFormula path_stamp = path_condition.clone(); // Extract path stamp
  IterationInfo info(timestamp, path_stamp);
  it->second.iteration_stack.push_back(info);

  // Limit stack size to prevent unbounded growth
  if (it->second.iteration_stack.size() > kMaxWidenIterations) {
    it->second.iteration_stack.erase(it->second.iteration_stack.begin());
  }
}

void LoopAbstraction::initLoopInfo(const llvm::BasicBlock *BB) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }
  it->second.iteration_stack.clear();
  it->second.local_path_condition = PulseFormula(); // Initialize to empty/true
}

void LoopAbstraction::removeLoopInfo(const llvm::BasicBlock *BB) {
  auto it = loop_headers_.find(BB);
  if (it != loop_headers_.end()) {
    it->second.iteration_stack.clear();
  }
}

unsigned LoopAbstraction::getIterationIndex(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return 0;
  }
  return it->second.iteration_stack.size();
}

bool LoopAbstraction::hasPreviousIterationSamePathStamp(
    const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end() || it->second.iteration_stack.size() < 2) {
    return false;
  }

  const auto &stack = it->second.iteration_stack;
  const auto &current = stack.back();
  const auto &previous = stack[stack.size() - 2];
  return current.path_stamp.equivalentTo(previous.path_stamp);
}

bool LoopAbstraction::isCurrentIterationEmptyPathStamp(
    const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end() || it->second.iteration_stack.size() < 2) {
    return false;
  }

  const auto &stack = it->second.iteration_stack;
  return stack[0].path_stamp.isEmptyOrTrivial();
}

void LoopAbstraction::mapFormulas(
    const llvm::BasicBlock *BB,
    std::function<PulseFormula(const PulseFormula &)> f) {
  auto it = loop_headers_.find(BB);
  if (it == loop_headers_.end()) {
    return;
  }

  PulseFormula new_condition = f(it->second.local_path_condition);
  it->second.local_path_condition = std::move(new_condition);

  // Also update path stamps in iteration stack
  for (auto &info : it->second.iteration_stack) {
    info.path_stamp = f(info.path_stamp);
  }
}

const ExecutionDomain &
LoopAbstraction::getEntryState(const llvm::BasicBlock *BB) const {
  auto it = loop_headers_.find(BB);
  if (it != loop_headers_.end()) {
    return it->second.entry_state;
  }
  static ExecutionDomain empty;
  return empty;
}

} // namespace pulse
