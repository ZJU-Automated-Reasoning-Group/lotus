/**
 * @file LockSetAnalysis.cpp
 * @brief Implementation of Lock Set Analysis
 *
 * This analysis tracks the set of locks held at each program point.
 * It computes two sets for each instruction:
 * 1. May-Lock Set: Locks that MIGHT be held. Used for deadlock detection and
 * reducing false positives.
 *    - Join operator: Union
 *    - Try-lock: Assumed successful
 * 2. Must-Lock Set: Locks that MUST be held. Used for proving mutual exclusion
 * (safety).
 *    - Join operator: Intersection
 *    - Try-lock: Assumed failed (safe approximation)
 *    - Release: Removes all aliasing locks to ensure soundness.
 */

#include "Analysis/Concurrency/LockSet/LockSetAnalysis.h"

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Analysis/Concurrency/Utils/RAIILockTracker.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <queue>
#include <set>
#include <stack>

#include <llvm/ADT/APInt.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;
using namespace mhp;

namespace {

bool isNonBinarySemaphoreOp(const ThreadAPI *thread_api,
                            const Instruction *inst) {
  return thread_api && inst && thread_api->isSemaphoreOp(inst) &&
         !thread_api->isBinarySemaphoreOp(inst);
}

void collectDefinedFunctionTargets(const Value *called,
                                   std::set<Function *> &callees,
                                   std::unordered_set<const Value *> &visited) {
  if (!called) {
    return;
  }
  called = called->stripPointerCasts();
  if (!visited.insert(called).second) {
    return;
  }

  if (const auto *func = dyn_cast<Function>(called)) {
    if (!func->isDeclaration()) {
      callees.insert(const_cast<Function *>(func));
    }
    return;
  }
  if (const auto *select = dyn_cast<SelectInst>(called)) {
    collectDefinedFunctionTargets(select->getTrueValue(), callees, visited);
    collectDefinedFunctionTargets(select->getFalseValue(), callees, visited);
    return;
  }
  if (const auto *phi = dyn_cast<PHINode>(called)) {
    for (const Value *incoming : phi->incoming_values()) {
      collectDefinedFunctionTargets(incoming, callees, visited);
    }
    return;
  }
  if (const auto *ce = dyn_cast<ConstantExpr>(called)) {
    if (ce->isCast() && ce->getNumOperands() > 0) {
      collectDefinedFunctionTargets(ce->getOperand(0), callees, visited);
    }
  }
}

bool getConstantOffsetPointerInfo(const Value *ptr, const Module *module,
                                  const Value *&base, int64_t &offset,
                                  uint64_t &size) {
  if (!ptr || !module) {
    return false;
  }

  ptr = ptr->stripPointerCasts();
  const auto *gep = dyn_cast<GEPOperator>(ptr);
  if (!gep) {
    return false;
  }

  const DataLayout &dl = module->getDataLayout();
  APInt ap_offset(dl.getIndexTypeSizeInBits(gep->getType()), 0, true);
  if (!gep->accumulateConstantOffset(dl, ap_offset)) {
    return false;
  }

  const Value *ptr_base = gep->getPointerOperand()->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(ptr_base, 32)) {
    ptr_base = underlying->stripPointerCasts();
  }

  Type *pointee_ty = gep->getResultElementType();
  if (!pointee_ty || !pointee_ty->isSized()) {
    return false;
  }

  base = ptr_base;
  offset = ap_offset.getSExtValue();
  size = dl.getTypeStoreSize(pointee_ty);
  return true;
}

bool areDisjointConstantOffsetPointers(const Value *lhs, const Value *rhs,
                                       const Module *module) {
  const Value *lhs_base = nullptr;
  const Value *rhs_base = nullptr;
  int64_t lhs_offset = 0;
  int64_t rhs_offset = 0;
  uint64_t lhs_size = 0;
  uint64_t rhs_size = 0;
  if (!getConstantOffsetPointerInfo(lhs, module, lhs_base, lhs_offset,
                                    lhs_size) ||
      !getConstantOffsetPointerInfo(rhs, module, rhs_base, rhs_offset,
                                    rhs_size)) {
    return false;
  }

  if (lhs_base != rhs_base) {
    return false;
  }

  const int64_t lhs_end = lhs_offset + static_cast<int64_t>(lhs_size);
  const int64_t rhs_end = rhs_offset + static_cast<int64_t>(rhs_size);
  return lhs_end <= rhs_offset || rhs_end <= lhs_offset;
}

} // namespace

// ============================================================================
// Construction and Analysis
// ============================================================================

LockSetAnalysis::LockSetAnalysis(Module &module)
    : m_module(&module), m_single_function(nullptr),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr),
      m_call_graph(nullptr) {}

LockSetAnalysis::LockSetAnalysis(Function &func)
    : m_module(nullptr), m_single_function(&func),
      m_thread_api(ThreadAPI::getThreadAPI()), m_alias_analysis(nullptr),
      m_call_graph(nullptr) {}

void LockSetAnalysis::analyze() {
  errs() << "Starting Lock Set Analysis...\n";

  m_may_locksets_entry.clear();
  m_may_locksets_exit.clear();
  m_must_locksets_entry.clear();
  m_must_locksets_exit.clear();
  m_may_read_locks_entry.clear();
  m_may_read_locks_exit.clear();
  m_may_write_locks_entry.clear();
  m_may_write_locks_exit.clear();
  m_must_read_locks_entry.clear();
  m_must_read_locks_exit.clear();
  m_must_write_locks_entry.clear();
  m_must_write_locks_exit.clear();
  m_all_locks.clear();
  m_lock_acquires.clear();
  m_lock_releases.clear();
  m_lock_try_acquires.clear();
  m_observed_lock_orders.clear();
  m_reentrant_locks.clear();
  m_raii_locks.clear();
  m_function_summaries.clear();

  if (m_module) {
    if (!m_call_graph) {
      m_owned_call_graph = std::make_unique<CallGraph>(*m_module);
      m_call_graph = m_owned_call_graph.get();
    }
    // Module-wide analysis
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        analyzeFunction(&func);
      }
    }
    computeInterproceduralLockSets();
  } else if (m_single_function) {
    // Single function analysis
    analyzeFunction(m_single_function);
  }

  // Identify locks after RAII lifetimes and final summaries have been computed.
  identifyLocks();

  // Track lock ordering for deadlock detection
  trackLockOrdering();

  errs() << "Lock Set Analysis Complete!\n";
  errs() << "Found " << m_all_locks.size() << " locks\n";
}

// ============================================================================
// Query Interface
// ============================================================================

LockSet LockSetAnalysis::getMayLockSetAt(const Instruction *inst) const {
  if (inst && !isa<CallBase>(inst) && !getRAIILocksReleasedAt(inst).empty()) {
    auto it_exit = m_may_locksets_exit.find(inst);
    if (it_exit != m_may_locksets_exit.end()) {
      return it_exit->second;
    }
  }
  // Entry map is authoritative when present and non-empty.
  auto it = m_may_locksets_entry.find(inst);
  if (it != m_may_locksets_entry.end() && !it->second.empty())
    return it->second;
  // Fallback: on a linear path, entry at inst = exit of prev (fixes worklist
  // order)
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_may_locksets_exit.find(prev);
    if (it_exit != m_may_locksets_exit.end())
      return it_exit->second;
  }
  // Fallback for block head: union of predecessors' terminator exit (fixes
  // merge/empty entry)
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term)
        continue;
      auto it_exit = m_may_locksets_exit.find(term);
      if (it_exit != m_may_locksets_exit.end())
        merged.insert(it_exit->second.begin(), it_exit->second.end());
    }
    if (!merged.empty())
      return merged;
  }
  return it != m_may_locksets_entry.end() ? it->second : LockSet();
}

LockSet LockSetAnalysis::getMayReadLockSetAt(const Instruction *inst) const {
  auto it = m_may_read_locks_entry.find(inst);
  if (it != m_may_read_locks_entry.end())
    return it->second;
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_may_read_locks_exit.find(prev);
    if (it_exit != m_may_read_locks_exit.end())
      return it_exit->second;
  }
  // Fallback for block head: union of predecessors' terminator exit
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term)
        continue;
      auto it_exit = m_may_read_locks_exit.find(term);
      if (it_exit != m_may_read_locks_exit.end())
        merged.insert(it_exit->second.begin(), it_exit->second.end());
    }
    if (!merged.empty())
      return merged;
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMayWriteLockSetAt(const Instruction *inst) const {
  auto it = m_may_write_locks_entry.find(inst);
  if (it != m_may_write_locks_entry.end() && !it->second.empty())
    return it->second;
  // Fallback: entry at inst = exit of prev on linear path (fixes worklist
  // order)
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_may_write_locks_exit.find(prev);
    if (it_exit != m_may_write_locks_exit.end())
      return it_exit->second;
  }
  // Fallback for block head: union of predecessors' terminator exit (fixes
  // DCL/singleton pattern where critical section starts at block entry)
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term)
        continue;
      auto it_exit = m_may_write_locks_exit.find(term);
      if (it_exit != m_may_write_locks_exit.end())
        merged.insert(it_exit->second.begin(), it_exit->second.end());
    }
    if (!merged.empty())
      return merged;
  }
  return it != m_may_write_locks_entry.end() ? it->second : LockSet();
}

LockSet LockSetAnalysis::getMustLockSetAt(const Instruction *inst) const {
  auto applyImpreciseBoundary = [this, inst](LockSet lockset) {
    for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
      lockset.erase(lock);
    }
    return lockset;
  };
  if (inst && !isa<CallBase>(inst) && !getRAIILocksReleasedAt(inst).empty()) {
    auto it_exit = m_must_locksets_exit.find(inst);
    if (it_exit != m_must_locksets_exit.end()) {
      return applyImpreciseBoundary(it_exit->second);
    }
  }
  auto it = m_must_locksets_entry.find(inst);
  if (it != m_must_locksets_entry.end())
    return applyImpreciseBoundary(it->second);
  // Fallback: entry at inst = exit of prev on linear path (so double-lock sees
  // held lock)
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_must_locksets_exit.find(prev);
    if (it_exit != m_must_locksets_exit.end())
      return applyImpreciseBoundary(it_exit->second);
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMustReadLockSetAt(const Instruction *inst) const {
  auto it = m_must_read_locks_entry.find(inst);
  if (it != m_must_read_locks_entry.end()) {
    LockSet result = it->second;
    for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
      result.erase(lock);
    }
    return result;
  }
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_must_read_locks_exit.find(prev);
    if (it_exit != m_must_read_locks_exit.end()) {
      LockSet result = it_exit->second;
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        result.erase(lock);
      }
      return result;
    }
  }
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    bool initialized = false;
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term) {
        continue;
      }
      auto it_exit = m_must_read_locks_exit.find(term);
      if (it_exit == m_must_read_locks_exit.end()) {
        continue;
      }
      if (!initialized) {
        merged = it_exit->second;
        initialized = true;
      } else {
        LockSet intersection;
        std::set_intersection(merged.begin(), merged.end(),
                              it_exit->second.begin(), it_exit->second.end(),
                              std::inserter(intersection, intersection.begin()));
        merged = std::move(intersection);
      }
    }
    if (initialized) {
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        merged.erase(lock);
      }
      return merged;
    }
  }
  return LockSet();
}

LockSet LockSetAnalysis::getMustWriteLockSetAt(const Instruction *inst) const {
  auto it = m_must_write_locks_entry.find(inst);
  if (it != m_must_write_locks_entry.end()) {
    LockSet result = it->second;
    for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
      result.erase(lock);
    }
    return result;
  }
  if (const Instruction *prev = inst->getPrevNode()) {
    auto it_exit = m_must_write_locks_exit.find(prev);
    if (it_exit != m_must_write_locks_exit.end()) {
      LockSet result = it_exit->second;
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        result.erase(lock);
      }
      return result;
    }
  }
  const BasicBlock *bb = inst->getParent();
  if (bb && inst == &bb->front()) {
    bool initialized = false;
    LockSet merged;
    for (const BasicBlock *pred : predecessors(bb)) {
      const Instruction *term = pred->getTerminator();
      if (!term) {
        continue;
      }
      auto it_exit = m_must_write_locks_exit.find(term);
      if (it_exit == m_must_write_locks_exit.end()) {
        continue;
      }
      if (!initialized) {
        merged = it_exit->second;
        initialized = true;
      } else {
        LockSet intersection;
        std::set_intersection(merged.begin(), merged.end(),
                              it_exit->second.begin(), it_exit->second.end(),
                              std::inserter(intersection, intersection.begin()));
        merged = std::move(intersection);
      }
    }
    if (initialized) {
      for (LockID lock : getImpreciseRAIILocksEndingAt(inst)) {
        merged.erase(lock);
      }
      return merged;
    }
  }
  return LockSet();
}

bool LockSetAnalysis::mayHoldLock(const Instruction *inst, LockID lock) const {
  auto lockset = getMayLockSetAt(inst);
  for (const auto *held_lock : lockset) {
    if (mayAlias(lock, held_lock)) {
      return true;
    }
  }
  return false;
}

bool LockSetAnalysis::mustHoldLock(const Instruction *inst, LockID lock) const {
  lock = getCanonicalLock(lock);
  auto lockset = getMustLockSetAt(inst);
  for (const auto *held_lock : lockset) {
    const LockID canonical_held = getCanonicalLock(held_lock);
    // Must queries require certainty. Accept either exact canonical equality or
    // a proven must-alias relation.
    if (canonical_held == lock)
      return true;
    if (m_alias_analysis && canonical_held && lock &&
        m_alias_analysis->mustAlias(canonical_held, lock))
      return true;
  }
  return false;
}

std::unordered_set<const Instruction *>
LockSetAnalysis::getInstructionsHoldingLock(LockID lock) const {
  std::unordered_set<const Instruction *> result;
  for (const auto &pair : m_may_locksets_entry) {
    if (pair.second.find(lock) != pair.second.end()) {
      result.insert(pair.first);
    }
  }
  return result;
}

bool LockSetAnalysis::mayHoldCommonLock(const Instruction *i1,
                                        const Instruction *i2) const {
  const Module *module =
      m_module ? m_module
               : (m_single_function ? m_single_function->getParent() : nullptr);
  auto commonWithDisjointFields = [this, module](const LockSet &a,
                                                 const LockSet &b) {
    for (const auto *lock : a) {
      if (b.find(lock) != b.end()) {
        return true;
      }
      for (const auto *lock2 : b) {
        if (areDisjointConstantOffsetPointers(lock, lock2, module)) {
          continue;
        }
        if (mayAlias(lock, lock2)) {
          return true;
        }
      }
    }
    return false;
  };
  LockSet r1 = getMayReadLockSetAt(i1), r2 = getMayReadLockSetAt(i2);
  LockSet w1 = getMayWriteLockSetAt(i1), w2 = getMayWriteLockSetAt(i2);
  return commonWithDisjointFields(w1, w2) || commonWithDisjointFields(r1, r2);
}

bool LockSetAnalysis::mustHoldCommonLock(const Instruction *i1,
                                         const Instruction *i2) const {
  auto matches = [this](LockID a, LockID b) {
    const LockID ca = getCanonicalLock(a);
    const LockID cb = getCanonicalLock(b);
    if (ca && cb && ca == cb)
      return true;
    return m_alias_analysis && ca && cb && m_alias_analysis->mustAlias(ca, cb);
  };

  LockSet write1 = getMustWriteLockSetAt(i1);
  LockSet write2 = getMustWriteLockSetAt(i2);

  for (LockID lock1 : write1) {
    for (LockID lock2 : write2) {
      if (!matches(lock1, lock2))
        continue;
      return true;
    }
  }

  return false;
}

LockSet LockSetAnalysis::getAllLocksInFunction(const Function *func) const {
  LockSet all_locks;
  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;
    if (isLockOperation(inst)) {
      LockID lock = getLockValue(inst);
      if (lock) {
        all_locks.insert(lock);
      }
    }
  }
  return all_locks;
}

std::vector<const Instruction *>
LockSetAnalysis::getLockAcquires(LockID lock) const {
  auto it = m_lock_acquires.find(lock);
  if (it != m_lock_acquires.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

std::vector<const Instruction *>
LockSetAnalysis::getLockReleases(LockID lock) const {
  auto it = m_lock_releases.find(lock);
  if (it != m_lock_releases.end()) {
    return it->second;
  }
  return std::vector<const Instruction *>();
}

// ============================================================================
// Advanced Queries
// ============================================================================

bool LockSetAnalysis::isReentrantLock(LockID lock) const {
  return m_reentrant_locks.find(lock) != m_reentrant_locks.end();
}

size_t LockSetAnalysis::getLockNestingDepth(const Instruction *inst) const {
  return getMayLockSetAt(inst).size();
}

bool LockSetAnalysis::areLocksOrderedConsistently(LockID lock1,
                                                  LockID lock2) const {
  bool found_12 = m_observed_lock_orders.find({lock1, lock2}) !=
                  m_observed_lock_orders.end();
  bool found_21 = m_observed_lock_orders.find({lock2, lock1}) !=
                  m_observed_lock_orders.end();

  // Consistent if only one order is observed
  return !(found_12 && found_21);
}

std::vector<std::pair<LockID, LockID>>
LockSetAnalysis::detectLockOrderInversions() const {
  std::vector<std::pair<LockID, LockID>> inversions;
  std::unordered_set<LockPair, LockPair::Hash> emitted;

  // Check all pairs of locks for order inversions
  for (const auto &pair1 : m_observed_lock_orders) {
    LockPair reverse{pair1.second, pair1.first};
    LockPair canonical = std::less<LockID>{}(pair1.first, pair1.second)
                             ? pair1
                             : LockPair{pair1.second, pair1.first};
    if (m_observed_lock_orders.find(reverse) != m_observed_lock_orders.end() &&
        emitted.insert(canonical).second) {
      // Found an inversion - both lock1->lock2 and lock2->lock1 exist
      inversions.push_back({canonical.first, canonical.second});
    }
  }

  return inversions;
}

// ============================================================================
// Statistics and Debugging
// ============================================================================

void LockSetAnalysis::Statistics::print(raw_ostream &os) const {
  os << "Lock Set Analysis Statistics:\n";
  os << "==============================\n";
  os << "Locks:                " << num_locks << "\n";
  os << "Lock Acquires:        " << num_acquires << "\n";
  os << "Lock Releases:        " << num_releases << "\n";
  os << "Try-Lock Operations:  " << num_try_acquires << "\n";
  os << "Max Depth:     " << max_nesting_depth << "\n";
  os << "Observed Reentrant Locks:     " << num_reentrant_locks << "\n";
  os << "Potential Deadlocks:  " << num_potential_deadlocks << "\n";
}

LockSetAnalysis::Statistics LockSetAnalysis::getStatistics() const {
  Statistics stats{};

  stats.num_locks = m_all_locks.size();

  stats.num_acquires = 0;
  for (const auto &pair : m_lock_acquires) {
    stats.num_acquires += pair.second.size();
  }

  stats.num_releases = 0;
  for (const auto &pair : m_lock_releases) {
    stats.num_releases += pair.second.size();
  }

  stats.num_try_acquires = 0;
  for (const auto &pair : m_lock_try_acquires) {
    stats.num_try_acquires += pair.second.size();
  }

  stats.max_nesting_depth = 0;
  for (const auto &pair : m_may_locksets_entry) {
    stats.max_nesting_depth =
        std::max(stats.max_nesting_depth, pair.second.size());
  }

  stats.num_reentrant_locks = m_reentrant_locks.size();
  stats.num_potential_deadlocks = detectLockOrderInversions().size();

  return stats;
}

void LockSetAnalysis::printStatistics(raw_ostream &os) const {
  auto stats = getStatistics();
  stats.print(os);
}

void LockSetAnalysis::printResults(raw_ostream &os) const {
  os << "\n=== Lock Set Analysis Results ===\n\n";

  printStatistics(os);

  os << "\n=== All Locks ===\n";
  for (const auto *lock : m_all_locks) {
    os << "Lock: ";
    lock->printAsOperand(os, false);
    os << "\n";

    auto acquires = getLockAcquires(lock);
    os << "  Acquires: " << acquires.size() << "\n";

    auto releases = getLockReleases(lock);
    os << "  Releases: " << releases.size() << "\n";

    if (isReentrantLock(lock)) {
      os << "  [REENTRANT]\n";
    }
  }

  // Print potential deadlocks
  auto inversions = detectLockOrderInversions();
  if (!inversions.empty()) {
    os << "\n=== Potential Deadlocks (Lock Order Inversions) ===\n";
    for (const auto &pair : inversions) {
      os << "Lock ";
      pair.first->printAsOperand(os, false);
      os << " and Lock ";
      pair.second->printAsOperand(os, false);
      os << "\n";
    }
  }
}

void LockSetAnalysis::printLockSetsForFunction(const Function *func,
                                               raw_ostream &os) const {
  os << "Lock Sets for Function: " << func->getName() << "\n";
  os << "=============================================\n";

  for (const_inst_iterator I = inst_begin(func), E = inst_end(func); I != E;
       ++I) {
    const Instruction *inst = &*I;

    auto may_locks = getMayLockSetAt(inst);
    auto must_locks = getMustLockSetAt(inst);

    if (!may_locks.empty() || isLockOperation(inst)) {
      os << "Instruction: ";
      inst->print(os);
      os << "\n";

      os << "  May-Locks: {";
      bool first = true;
      for (const auto *lock : may_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n";

      os << "  Must-Locks: {";
      first = true;
      for (const auto *lock : must_locks) {
        if (!first)
          os << ", ";
        lock->printAsOperand(os, false);
        first = false;
      }
      os << "}\n\n";
    }
  }
}

void LockSetAnalysis::print(raw_ostream &os) const { printResults(os); }

// ============================================================================
// Visualization
// ============================================================================

void LockSetAnalysis::dumpLockGraph(const std::string &filename) const {
  std::error_code EC;
  raw_fd_ostream file(filename, EC, sys::fs::OF_None);

  if (EC) {
    errs() << "Error opening file " << filename << ": " << EC.message() << "\n";
    return;
  }

  file << "digraph LockGraph {\n";
  file << "  rankdir=LR;\n";
  file << "  node [shape=box];\n\n";

  // Create nodes for locks
  size_t id = 0;
  std::unordered_map<LockID, size_t> lock_ids;
  for (const auto *lock : m_all_locks) {
    lock_ids[lock] = id;
    file << "  lock" << id << " [label=\"";
    lock->printAsOperand(file, false);
    file << "\"];\n";
    id++;
  }

  file << "\n";

  // Create edges for lock ordering
  for (const auto &pair : m_observed_lock_orders) {
    auto it1 = lock_ids.find(pair.first);
    auto it2 = lock_ids.find(pair.second);
    if (it1 != lock_ids.end() && it2 != lock_ids.end()) {
      file << "  lock" << it1->second << " -> lock" << it2->second;

      // Highlight inversions in red
      LockPair reverse{pair.second, pair.first};
      if (m_observed_lock_orders.find(reverse) !=
          m_observed_lock_orders.end()) {
        file << " [color=red, style=bold]";
      }

      file << ";\n";
    }
  }

  file << "}\n";
  file.close();

  errs() << "Lock graph dumped to " << filename << "\n";
}

// ============================================================================
// Analysis Implementation
// ============================================================================

void LockSetAnalysis::analyzeFunction(Function *func) {
  if (!func || func->isDeclaration())
    return;

  // First, analyze RAII lock lifetimes in this function
  RAIILock::RAIILockTracker raii_tracker;
  raii_tracker.analyzeFunction(func);

  // Store RAII lock info for use during transfer function
  m_raii_locks[func] = raii_tracker.getAllLockLifetimes();

  computeIntraproceduralLockSets(func);
}

void LockSetAnalysis::computeIntraproceduralLockSets(Function *func) {
  auto clearFunctionFacts = [&](Function *target) {
    if (!target) {
      return;
    }
    for (Instruction &inst : instructions(target)) {
      const Instruction *key = &inst;
      m_may_locksets_entry.erase(key);
      m_may_locksets_exit.erase(key);
      m_must_locksets_entry.erase(key);
      m_must_locksets_exit.erase(key);
      m_may_read_locks_entry.erase(key);
      m_may_read_locks_exit.erase(key);
      m_may_write_locks_entry.erase(key);
      m_may_write_locks_exit.erase(key);
      m_must_read_locks_entry.erase(key);
      m_must_read_locks_exit.erase(key);
      m_must_write_locks_entry.erase(key);
      m_must_write_locks_exit.erase(key);
    }
  };

  clearFunctionFacts(func);
  const LockSet all_locks_in_function = getAllLocksInFunction(func);

  // Standard forward dataflow analysis using a worklist algorithm.
  //
  // Lattice: Sets of LockIDs.
  // - May-Analysis: Union (Join).
  //   Effect: Collects all locks that *might* be held on any path to this
  //   point.
  // - Must-Analysis: Intersection (Join).
  //   Effect: Collects all locks that *must* be held on all paths to this
  //   point.

  // Worklist algorithm for dataflow analysis
  std::queue<const Instruction *> worklist;
  std::set<const Instruction *> in_worklist;

  const Instruction *entry = &func->getEntryBlock().front();
  // Do not pre-initialize entry's maps: then the first time we process it we
  // have had_entry=false and we add successors (otherwise we never propagate
  // when the computed value is empty).
  worklist.push(entry);
  in_worklist.insert(entry);

  while (!worklist.empty()) {
    const Instruction *inst = worklist.front();
    worklist.pop();
    in_worklist.erase(inst);

    std::vector<LockSet> may_inputs, must_inputs;
    std::vector<LockSet> may_read_inputs, may_write_inputs;
    std::vector<LockSet> must_read_inputs, must_write_inputs;

    if (inst == entry) {
      may_inputs.push_back(LockSet());
      must_inputs.push_back(LockSet());
      may_read_inputs.push_back(LockSet());
      may_write_inputs.push_back(LockSet());
      must_read_inputs.push_back(LockSet());
      must_write_inputs.push_back(LockSet());
    } else {
      const BasicBlock *bb = inst->getParent();
      if (inst == &bb->front()) {
        for (const BasicBlock *pred : predecessors(bb)) {
          const Instruction *pred_term = pred->getTerminator();
          if (pred_term) {
            auto it_may = m_may_locksets_exit.find(pred_term);
            auto it_must = m_must_locksets_exit.find(pred_term);
            auto it_mr = m_may_read_locks_exit.find(pred_term);
            auto it_mw = m_may_write_locks_exit.find(pred_term);
            auto it_ur = m_must_read_locks_exit.find(pred_term);
            auto it_uw = m_must_write_locks_exit.find(pred_term);

            if (it_may != m_may_locksets_exit.end()) {
              may_inputs.push_back(it_may->second);
              must_inputs.push_back(it_must != m_must_locksets_exit.end() ? it_must->second : all_locks_in_function);
              may_read_inputs.push_back(it_mr != m_may_read_locks_exit.end() ? it_mr->second : LockSet());
              may_write_inputs.push_back(it_mw != m_may_write_locks_exit.end() ? it_mw->second : LockSet());
              must_read_inputs.push_back(it_ur != m_must_read_locks_exit.end() ? it_ur->second : all_locks_in_function);
              must_write_inputs.push_back(it_uw != m_must_write_locks_exit.end() ? it_uw->second : all_locks_in_function);
            } else {
              // Predecessor not yet processed. For must-analysis, we must be conservative.
              // If we haven't visited the predecessor, we can't assume any locks are held.
              // However, intersection with "all locks" is the neutral element.
              must_inputs.push_back(all_locks_in_function);
              must_read_inputs.push_back(all_locks_in_function);
              must_write_inputs.push_back(all_locks_in_function);
            }
          }
        }
      } else {
        const Instruction *prev = inst->getPrevNode();
        if (prev) {
          auto it_may = m_may_locksets_exit.find(prev);
          if (it_may != m_may_locksets_exit.end()) {
            may_inputs.push_back(it_may->second);
            auto it_must = m_must_locksets_exit.find(prev);
            must_inputs.push_back(it_must != m_must_locksets_exit.end() ? it_must->second : all_locks_in_function);
            auto it_mr = m_may_read_locks_exit.find(prev);
            may_read_inputs.push_back(it_mr != m_may_read_locks_exit.end() ? it_mr->second : LockSet());
            auto it_mw = m_may_write_locks_exit.find(prev);
            may_write_inputs.push_back(it_mw != m_may_write_locks_exit.end() ? it_mw->second : LockSet());
            auto it_ur = m_must_read_locks_exit.find(prev);
            must_read_inputs.push_back(it_ur != m_must_read_locks_exit.end() ? it_ur->second : all_locks_in_function);
            auto it_uw = m_must_write_locks_exit.find(prev);
            must_write_inputs.push_back(it_uw != m_must_write_locks_exit.end() ? it_uw->second : all_locks_in_function);
          }
        }
      }
    }
    LockSet may_read_in =
        may_read_inputs.empty() ? LockSet() : merge(may_read_inputs, false);
    LockSet may_write_in =
        may_write_inputs.empty() ? LockSet() : merge(may_write_inputs, false);
    LockSet must_read_in =
        must_read_inputs.empty() ? LockSet() : merge(must_read_inputs, true);
    LockSet must_write_in =
        must_write_inputs.empty() ? LockSet() : merge(must_write_inputs, true);
    LockSet may_in = may_read_in;
    may_in.insert(may_write_in.begin(), may_write_in.end());
    LockSet must_in = must_read_in;
    must_in.insert(must_write_in.begin(), must_write_in.end());

    LockSet may_read_out, may_write_out, must_read_out, must_write_out;
    transferReadWrite(inst, may_read_in, may_write_in, may_read_out,
                      may_write_out, false);
    transferReadWrite(inst, must_read_in, must_write_in, must_read_out,
                      must_write_out, true);

    LockSet may_out = transfer(inst, may_in, false);
    LockSet must_out = transfer(inst, must_in, true);

    // First time we see this instruction we must propagate (otherwise we never
    // add successors when the computed value equals the default empty set).
    bool had_entry = m_may_locksets_entry.count(inst);
    bool changed = !had_entry;
    if (m_may_read_locks_entry[inst] != may_read_in) {
      m_may_read_locks_entry[inst] = may_read_in;
      changed = true;
    }
    if (m_may_read_locks_exit[inst] != may_read_out) {
      m_may_read_locks_exit[inst] = may_read_out;
      changed = true;
    }
    if (m_may_write_locks_entry[inst] != may_write_in) {
      m_may_write_locks_entry[inst] = may_write_in;
      changed = true;
    }
    if (m_may_write_locks_exit[inst] != may_write_out) {
      m_may_write_locks_exit[inst] = may_write_out;
      changed = true;
    }
    if (m_must_read_locks_entry[inst] != must_read_in) {
      m_must_read_locks_entry[inst] = must_read_in;
      changed = true;
    }
    if (m_must_read_locks_exit[inst] != must_read_out) {
      m_must_read_locks_exit[inst] = must_read_out;
      changed = true;
    }
    if (m_must_write_locks_entry[inst] != must_write_in) {
      m_must_write_locks_entry[inst] = must_write_in;
      changed = true;
    }
    if (m_must_write_locks_exit[inst] != must_write_out) {
      m_must_write_locks_exit[inst] = must_write_out;
      changed = true;
    }
    if (m_may_locksets_entry[inst] != may_in) {
      m_may_locksets_entry[inst] = may_in;
      changed = true;
    }
    if (m_may_locksets_exit[inst] != may_out) {
      m_may_locksets_exit[inst] = may_out;
      changed = true;
    }
    if (m_must_locksets_entry[inst] != must_in) {
      m_must_locksets_entry[inst] = must_in;
      changed = true;
    }
    if (m_must_locksets_exit[inst] != must_out) {
      m_must_locksets_exit[inst] = must_out;
      changed = true;
    }

    // Add successors to worklist if changed
    if (changed) {
      // Add next instruction in block
      if (inst->getNextNode()) {
        const Instruction *next = inst->getNextNode();
        if (in_worklist.find(next) == in_worklist.end()) {
          worklist.push(next);
          in_worklist.insert(next);
        }
      } else if (inst->isTerminator()) {
        // Add successor blocks
        const BasicBlock *bb = inst->getParent();
        for (const BasicBlock *succ_bb : successors(bb)) {
          const Instruction *succ = &succ_bb->front();
          if (in_worklist.find(succ) == in_worklist.end()) {
            worklist.push(succ);
            in_worklist.insert(succ);
          }
        }
      }
    }
  }
}

void LockSetAnalysis::computeInterproceduralLockSets() {
  if (!m_call_graph) {
    errs() << "Warning: CallGraph not available. Skipping interprocedural "
              "analysis.\n";
    return;
  }

  errs() << "Computing interprocedural lock sets using CallGraph...\n";

  // Perform bottom-up traversal of call graph to compute function summaries
  bottomUpTraversal();

  // Re-analyze each function with interprocedural context
  for (Function &func : *m_module) {
    if (!func.isDeclaration()) {
      analyzeFunction(&func);
    }
  }

  for (auto &entry : m_function_summaries) {
    entry.second.is_analyzed = false;
  }
  bottomUpTraversal();

  errs() << "Interprocedural lock set analysis complete.\n";
}

LockSet LockSetAnalysis::transfer(const Instruction *inst,
                                  const LockSet &in_set, bool is_must) const {
  LockSet out_set = in_set;
  auto eraseReleasedLocks = [&](const std::vector<LockID> &locks) {
    for (LockID lock : locks) {
      out_set.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove;
        for (const auto *held : out_set) {
          if (mayAlias(held, lock)) {
            to_remove.insert(held);
          }
        }
        for (const auto *held : to_remove) {
          out_set.erase(held);
        }
      }
    }
  };

  std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst);
  if (!raii_releases.empty()) {
    eraseReleasedLocks(raii_releases);
    return out_set;
  }
  if (is_must) {
    eraseReleasedLocks(getImpreciseRAIILocksEndingAt(inst));
  }

  const CallBase *call = dyn_cast<CallBase>(inst);
  ThreadAPI::TD_TYPE call_type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return out_set;
  }
  const bool raw_lock_api = call_type == ThreadAPI::TD_ACQUIRE ||
                            call_type == ThreadAPI::TD_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_RWLOCK_RDLOCK ||
                            call_type == ThreadAPI::TD_RWLOCK_WRLOCK ||
                            call_type == ThreadAPI::TD_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_ACQUIRE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN ||
                            call_type == ThreadAPI::TD_KERNEL_READ_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_READ ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_WRITE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP ||
                            call_type == ThreadAPI::TD_KERNEL_READ_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP_READ ||
                            call_type == ThreadAPI::TD_KERNEL_UP_WRITE;

  // Check if this is a lock operation
  if (m_thread_api->isTDAcquire(inst) && raw_lock_api) {
    // Try-lock may fail, so it is only added to the may-set.
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      LockID lock = getLockValue(inst);
      if (lock) {
        out_set.insert(lock);
      }
    }
  } else if (m_thread_api->isTDRelease(inst)) {
    // Lock release - remove lock from set
    LockID lock = getLockValue(inst);
    if (lock) {
      out_set.erase(lock);

      // Only in must-analysis do we safely drop aliasing locks.
      // In may-analysis this would under-approximate the held set.
      // For Must-Lock analysis: If we release 'lock', we must also remove any
      // lock 'l' that *might* be an alias of 'lock'. If we kept 'l', we might
      // falsely believe we still hold 'l' when we actually released it via
      // 'lock'.
      if (is_must && m_alias_analysis) {
        LockSet to_remove;
        for (const auto *l : out_set) {
          if (mayAlias(l, lock)) {
            to_remove.insert(l);
          }
        }
        for (const auto *l : to_remove) {
          out_set.erase(l);
        }
      }
    }
  } else if (m_thread_api->isTDCondWait(inst)) {
    // pthread_cond_wait atomically releases the mutex and re-acquires on
    // return; lock set unchanged.
  } else if (call) {
    ThreadAPI::TD_TYPE type = call_type;

    // Handle modern C++ synchronization primitives
    switch (type) {
    case ThreadAPI::TD_SHARED_RDLOCK:
    case ThreadAPI::TD_SHARED_WRLOCK:
      // Handled in transferReadWrite, but also update combined set
      if (LockID lock = getLockValue(inst))
        out_set.insert(lock);
      return out_set;

    case ThreadAPI::TD_SHARED_UNLOCK:
      // Release both read and write locks
      if (LockID lock = getLockValue(inst)) {
        out_set.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove;
          for (const auto *l : out_set)
            if (mayAlias(l, lock))
              to_remove.insert(l);
          for (const auto *l : to_remove)
            out_set.erase(l);
        }
      }
      return out_set;

    // RAII lock constructors (acquire)
    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    case ThreadAPI::TD_SHARED_LOCK_CTOR: {
      auto shouldAddAtCtor = [&](LockID lock,
                                 RAIILock::OwnershipKind ownership) {
        switch (ownership) {
        case RAIILock::OwnershipKind::Immediate:
          return true;
        case RAIILock::OwnershipKind::Deferred:
          return false;
        case RAIILock::OwnershipKind::Try:
          return !is_must;
        case RAIILock::OwnershipKind::Adopt:
          for (const auto *held : in_set) {
            if (held == lock || mayAlias(held, lock)) {
              return true;
            }
          }
          return false;
        case RAIILock::OwnershipKind::Unknown:
          return !is_must;
        }
        return false;
      };

      // Use RAII tracker to get the underlying mutex
      const Function *parent_func = inst->getFunction();
      auto raii_it = m_raii_locks.find(parent_func);
      if (raii_it != m_raii_locks.end()) {
        for (const auto &raii_entry : raii_it->second) {
          const RAIILock::LockLifetime &lifetime = raii_entry.second;
          if (lifetime.constructor == call &&
              !lifetime.underlyingLocks.empty()) {
            for (const Value *underlying : lifetime.underlyingLocks) {
              if (LockID lock = getCanonicalLock(underlying)) {
                if (shouldAddAtCtor(lock, lifetime.ownership)) {
                  out_set.insert(lock);
                }
              }
            }
            return out_set;
          }
        }
      }
      // Fallback to argument-based detection
      RAIILock::OwnershipKind fallback_ownership =
          RAIILock::RAIILockTracker::getOwnershipKind(call);
      for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(idx))) {
          if (shouldAddAtCtor(lock, fallback_ownership)) {
            out_set.insert(lock);
          }
        }
      }
    }
      return out_set;

    // RAII lock destructors (release)
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_SHARED_LOCK_DTOR:
      // Use RAII tracker to find which lock is being released
      {
        const Function *parent_func = inst->getFunction();
        auto raii_it = m_raii_locks.find(parent_func);
        if (raii_it != m_raii_locks.end()) {
          // Find the RAII lock object for this destructor
          for (const auto &raii_entry : raii_it->second) {
            const RAIILock::LockLifetime &lifetime = raii_entry.second;
            // Check if this destructor call corresponds to this lock lifetime
            for (const Instruction *dtor : lifetime.destructors) {
              if (dtor == inst && !lifetime.underlyingLocks.empty()) {
                for (const Value *underlying : lifetime.underlyingLocks) {
                  LockID lock = getCanonicalLock(underlying);
                  if (!lock) {
                    continue;
                  }
                  out_set.erase(lock);
                  if (is_must && m_alias_analysis) {
                    LockSet to_remove;
                    for (const auto *l : out_set) {
                      if (mayAlias(l, lock)) {
                        to_remove.insert(l);
                      }
                    }
                    for (const auto *l : to_remove) {
                      out_set.erase(l);
                    }
                  }
                }
                return out_set;
              }
            }
          }
        }
        if (LockID lock = getCppWrapperLockValue(inst)) {
          out_set.erase(lock);
          if (is_must && m_alias_analysis) {
            LockSet to_remove;
            for (const auto *l : out_set) {
              if (mayAlias(l, lock)) {
                to_remove.insert(l);
              }
            }
            for (const auto *l : to_remove) {
              out_set.erase(l);
            }
          }
          return out_set;
        }
        // Fallback: if we couldn't match this dtor to any RAII lifetime (e.g.
        // indirect call or unrecognized wrapper), clear entire must-set for
        // soundness: we must not claim any lock is still held after an
        // unknown release.
        if (is_must) {
          out_set.clear();
        }
      }
      return out_set;

    // unique_lock manual operations
    case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
      // Manual lock() call on unique_lock
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.insert(lock);
      }
      return out_set;

    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
      // Manual unlock() call on unique_lock
      if (LockID lock = getCppWrapperLockValue(inst)) {
        out_set.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove;
          for (const auto *l : out_set)
            if (mayAlias(l, lock))
              to_remove.insert(l);
          for (const auto *l : to_remove)
            out_set.erase(l);
        }
      }
      return out_set;

    // Synchronization primitives (don't hold locks, but create sync edges)
    case ThreadAPI::TD_CALL_ONCE:
    case ThreadAPI::TD_FUTURE_GET:
    case ThreadAPI::TD_FUTURE_WAIT:
    case ThreadAPI::TD_PROMISE_SET:
    case ThreadAPI::TD_LATCH_WAIT:
    case ThreadAPI::TD_LATCH_ARRIVE_WAIT:
    case ThreadAPI::TD_BARRIER_ARRIVE_WAIT:
    case ThreadAPI::TD_BARRIER_WAIT_CPP20:
    case ThreadAPI::TD_OMP_TASKWAIT:
    case ThreadAPI::TD_OMP_TASKWAIT_DEPS:
    case ThreadAPI::TD_OMP_TASKGROUP_END:
    case ThreadAPI::TD_OMP_FLUSH:
      // These are synchronization points but don't modify lock sets
      return out_set;

    default:
      break;
    }

    // Handle regular function calls with interprocedural summaries (existing
    // code continues) Try-lock is handled above (not added to set). Handle
    // other calls.
    if (!m_thread_api->isTDAcquire(call) && !m_thread_api->isTDRelease(call) &&
        !m_thread_api->isTDCondWait(call)) {
      // Handle regular function calls with interprocedural summaries.
      auto callees = getCallees(call);
      if (callees.empty()) {
        if (is_must && shouldInvalidateMustLockState(call)) {
          out_set.clear();
        }
        return out_set;
      }
      std::vector<LockSet> callee_results;
      for (Function *callee : callees) {
        if (!callee || callee->isDeclaration())
          continue;
        LockSet candidate = out_set;
        auto it = m_function_summaries.find(callee);
        if (it != m_function_summaries.end() && it->second.is_analyzed) {
          if (!is_must) {
            LockSet may_only = candidate;
            LockSet must_dummy = candidate;
            applyFunctionSummary(call, callee, may_only, must_dummy);
            candidate = std::move(may_only);
          } else {
            LockSet may_dummy = candidate;
            LockSet must_only = candidate;
            applyFunctionSummary(call, callee, may_dummy, must_only);
            candidate = std::move(must_only);
          }
        }
        callee_results.push_back(std::move(candidate));
      }
      if (!callee_results.empty()) {
        out_set = merge(callee_results, is_must);
      }
      if (is_must && shouldInvalidateMustLockState(call)) {
        out_set.clear();
      }
    }
  }

  return out_set;
}

void LockSetAnalysis::transferReadWrite(const Instruction *inst,
                                        const LockSet &in_read,
                                        const LockSet &in_write,
                                        LockSet &out_read, LockSet &out_write,
                                        bool is_must) const {
  out_read = in_read;
  out_write = in_write;
  auto eraseReleasedLocks = [&](const std::vector<LockID> &locks) {
    for (LockID lock : locks) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *held : out_read) {
          if (mayAlias(held, lock)) {
            to_remove_r.insert(held);
          }
        }
        for (const auto *held : out_write) {
          if (mayAlias(held, lock)) {
            to_remove_w.insert(held);
          }
        }
        for (const auto *held : to_remove_r) {
          out_read.erase(held);
        }
        for (const auto *held : to_remove_w) {
          out_write.erase(held);
        }
      }
    }
  };

  std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst);
  if (!raii_releases.empty()) {
    eraseReleasedLocks(raii_releases);
    return;
  }
  if (is_must) {
    eraseReleasedLocks(getImpreciseRAIILocksEndingAt(inst));
  }

  const CallBase *call = dyn_cast<CallBase>(inst);
  ThreadAPI::TD_TYPE call_type =
      call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
  if (isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return;
  }
  const bool raw_lock_api = call_type == ThreadAPI::TD_ACQUIRE ||
                            call_type == ThreadAPI::TD_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_RWLOCK_RDLOCK ||
                            call_type == ThreadAPI::TD_RWLOCK_WRLOCK ||
                            call_type == ThreadAPI::TD_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_ACQUIRE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_RELEASE ||
                            call_type == ThreadAPI::TD_SEMAPHORE_TRY_ACQUIRE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_TRYLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN ||
                            call_type == ThreadAPI::TD_KERNEL_READ_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_LOCK ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_READ ||
                            call_type == ThreadAPI::TD_KERNEL_DOWN_WRITE ||
                            call_type == ThreadAPI::TD_KERNEL_SPIN_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_MUTEX_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP ||
                            call_type == ThreadAPI::TD_KERNEL_READ_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_WRITE_UNLOCK ||
                            call_type == ThreadAPI::TD_KERNEL_UP_READ ||
                            call_type == ThreadAPI::TD_KERNEL_UP_WRITE;

  if (m_thread_api->isReadLockAcquire(inst)) {
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      LockID lock = getLockValue(inst);
      if (lock)
        out_read.insert(lock);
    }
  } else if (m_thread_api->isWriteLockAcquire(inst)) {
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      LockID lock = getLockValue(inst);
      if (lock) {
        out_write.insert(lock);
        for (const auto *l : in_read) {
          if (mayAlias(l, lock))
            out_read.erase(l);
        }
      }
    }
  } else if (m_thread_api->isTDAcquire(inst) && raw_lock_api) {
    // Try-lock may fail; add only to the may-set.
    if (!m_thread_api->isTryLock(inst) || !is_must) {
      LockID lock = getLockValue(inst);
      if (lock)
        out_write.insert(lock);
    }
  } else if (m_thread_api->isTDCondWait(inst)) {
    // pthread_cond_wait releases then re-acquires the mutex; read/write sets
    // unchanged.
  } else if (m_thread_api->isTDRelease(inst)) {
    LockID lock = getLockValue(inst);
    if (lock) {
      out_read.erase(lock);
      out_write.erase(lock);
      if (is_must && m_alias_analysis) {
        LockSet to_remove_r, to_remove_w;
        for (const auto *l : out_read) {
          if (mayAlias(l, lock))
            to_remove_r.insert(l);
        }
        for (const auto *l : out_write) {
          if (mayAlias(l, lock))
            to_remove_w.insert(l);
        }
        for (const auto *l : to_remove_r)
          out_read.erase(l);
        for (const auto *l : to_remove_w)
          out_write.erase(l);
      }
    }
  } else if (call) {
    ThreadAPI::TD_TYPE type = call_type;

    switch (type) {
    case ThreadAPI::TD_SHARED_RDLOCK:
      // std::shared_mutex::lock_shared - acquire read lock
      if (call->arg_size() >= 1) {
        LockID lock = getCanonicalLock(call->getArgOperand(0));
        if (lock)
          out_read.insert(lock);
      }
      return;

    case ThreadAPI::TD_SHARED_WRLOCK:
      // std::shared_mutex::lock - acquire write lock
      if (call->arg_size() >= 1) {
        LockID lock = getCanonicalLock(call->getArgOperand(0));
        if (lock) {
          out_write.insert(lock);
          // Remove any read locks on the same mutex
          for (const auto *l : in_read) {
            if (mayAlias(l, lock))
              out_read.erase(l);
          }
        }
      }
      return;

    case ThreadAPI::TD_SHARED_UNLOCK:
      // std::shared_mutex::unlock[_shared] - release lock
      if (call->arg_size() >= 1) {
        LockID lock = getCanonicalLock(call->getArgOperand(0));
        if (lock) {
          out_read.erase(lock);
          out_write.erase(lock);
          if (is_must && m_alias_analysis) {
            LockSet to_remove_r, to_remove_w;
            for (const auto *l : out_read)
              if (mayAlias(l, lock))
                to_remove_r.insert(l);
            for (const auto *l : out_write)
              if (mayAlias(l, lock))
                to_remove_w.insert(l);
            for (const auto *l : to_remove_r)
              out_read.erase(l);
            for (const auto *l : to_remove_w)
              out_write.erase(l);
          }
        }
      }
      return;

    case ThreadAPI::TD_SHARED_LOCK_CTOR:
      // std::shared_lock constructor - acquire read lock
      {
        RAIILock::OwnershipKind ownership =
            RAIILock::RAIILockTracker::getOwnershipKind(call);
        bool should_add =
            ownership == RAIILock::OwnershipKind::Immediate ||
            (!is_must && (ownership == RAIILock::OwnershipKind::Try ||
                          ownership == RAIILock::OwnershipKind::Unknown));
        if (!should_add) {
          if (ownership != RAIILock::OwnershipKind::Adopt) {
            break;
          }
        }
      }
      for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(idx))) {
          if (RAIILock::RAIILockTracker::getOwnershipKind(call) ==
              RAIILock::OwnershipKind::Adopt) {
            bool held = false;
            for (const auto *candidate : in_read) {
              if (mayAlias(candidate, lock)) {
                held = true;
                break;
              }
            }
            if (!held && is_must) {
              continue;
            }
            if (!held) {
              continue;
            }
          }
          out_read.insert(lock);
        }
      }
      return;

    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
      // These acquire write/exclusive locks
      {
        RAIILock::OwnershipKind ownership =
            RAIILock::RAIILockTracker::getOwnershipKind(call);
        bool should_add =
            ownership == RAIILock::OwnershipKind::Immediate ||
            (!is_must && (ownership == RAIILock::OwnershipKind::Try ||
                          ownership == RAIILock::OwnershipKind::Unknown));
        if (ownership == RAIILock::OwnershipKind::Deferred) {
          should_add = false;
        }
        if (!should_add) {
          if (ownership != RAIILock::OwnershipKind::Adopt) {
            break;
          }
        }
      }
      for (unsigned idx = 1; idx < call->arg_size(); ++idx) {
        if (LockID lock = getCanonicalLock(call->getArgOperand(idx))) {
          if (RAIILock::RAIILockTracker::getOwnershipKind(call) ==
              RAIILock::OwnershipKind::Adopt) {
            bool held = false;
            for (const auto *candidate : in_write) {
              if (mayAlias(candidate, lock)) {
                held = true;
                break;
              }
            }
            if (!held && is_must) {
              continue;
            }
            if (!held) {
              continue;
            }
          }
          out_write.insert(lock);
        }
      }
      return;

    case ThreadAPI::TD_SHARED_LOCK_DTOR:
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK: {
      std::vector<LockID> locks =
          getUnderlyingRAIILocks(inst, call->getArgOperand(0));
      if (locks.empty()) {
        if (LockID lock = getCppWrapperLockValue(inst)) {
          locks.push_back(lock);
        }
      }
      if (locks.empty() && is_must) {
        out_read.clear();
        out_write.clear();
        break;
      }
      for (LockID lock : locks) {
        out_read.erase(lock);
        out_write.erase(lock);
        if (is_must && m_alias_analysis) {
          LockSet to_remove_r, to_remove_w;
          for (const auto *l : out_read)
            if (mayAlias(l, lock))
              to_remove_r.insert(l);
          for (const auto *l : out_write)
            if (mayAlias(l, lock))
              to_remove_w.insert(l);
          for (const auto *l : to_remove_r)
            out_read.erase(l);
          for (const auto *l : to_remove_w)
            out_write.erase(l);
        }
      }
      return;
    }

    case ThreadAPI::TD_UNIQUE_LOCK_LOCK: {
      std::vector<LockID> locks =
          getUnderlyingRAIILocks(inst, call->getArgOperand(0));
      if (locks.empty()) {
        if (LockID lock = getCppWrapperLockValue(inst)) {
          locks.push_back(lock);
        }
      }
      for (LockID lock : locks) {
        out_write.insert(lock);
      }
      return;
    }

    default:
      break;
    }

    auto applySummaryToReadWrite = [&](const Function *callee,
                                      LockSet &candidate_read,
                                      LockSet &candidate_write) -> bool {
      auto it = m_function_summaries.find(callee);
      if (it == m_function_summaries.end() || !it->second.is_analyzed) {
        return false;
      }

      const FunctionSummary &summary = it->second;
      if (!is_must) {
        candidate_read.insert(summary.may_read_acquire_delta.begin(),
                              summary.may_read_acquire_delta.end());
        candidate_write.insert(summary.may_write_acquire_delta.begin(),
                               summary.may_write_acquire_delta.end());
      } else {
        candidate_read.insert(summary.must_read_acquire_delta.begin(),
                              summary.must_read_acquire_delta.end());
        candidate_write.insert(summary.must_write_acquire_delta.begin(),
                               summary.must_write_acquire_delta.end());
        for (LockID lock : summary.may_release_delta) {
          candidate_read.erase(lock);
          candidate_write.erase(lock);
        }
      }

      for (LockID lock : summary.must_release_delta) {
        candidate_read.erase(lock);
        candidate_write.erase(lock);
      }
      return true;
    };

    auto callees = getCallees(call);
    std::vector<LockSet> read_results;
    std::vector<LockSet> write_results;
    for (Function *callee : callees) {
      if (!callee || callee->isDeclaration()) {
        continue;
      }
      LockSet candidate_read = out_read;
      LockSet candidate_write = out_write;
      (void)applySummaryToReadWrite(callee, candidate_read, candidate_write);
      read_results.push_back(std::move(candidate_read));
      write_results.push_back(std::move(candidate_write));
    }

    if (!read_results.empty()) {
      out_read = merge(read_results, is_must);
      out_write = merge(write_results, is_must);
    }

    if (is_must && shouldInvalidateMustLockState(call)) {
      out_read.clear();
      out_write.clear();
    }
  }
}

LockSet LockSetAnalysis::merge(const std::vector<LockSet> &sets,
                               bool is_must) const {
  if (sets.empty()) {
    return LockSet();
  }

  if (is_must) {
    // Must-analysis: intersection
    auto matchesLock = [this](LockID lhs, LockID rhs) {
      const LockID clhs = getCanonicalLock(lhs);
      const LockID crhs = getCanonicalLock(rhs);
      if (clhs && crhs && clhs == crhs) {
        return true;
      }
      return m_alias_analysis && clhs && crhs &&
             m_alias_analysis->mustAlias(clhs, crhs);
    };

    LockSet result = sets[0];
    for (size_t i = 1; i < sets.size(); ++i) {
      LockSet intersection;
      for (LockID lhs : result) {
        for (LockID rhs : sets[i]) {
          if (matchesLock(lhs, rhs)) {
            intersection.insert(getCanonicalLock(lhs));
            break;
          }
        }
      }
      result = intersection;
    }
    return result;
  } // May-analysis: union
  LockSet result;
  for (const auto &set : sets) {
    result.insert(set.begin(), set.end());
  }
  return result;
}

void LockSetAnalysis::identifyLocks() {
  auto process_func = [this](Function &func) {
    for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
      Instruction *inst = &*I;
      const auto *call = dyn_cast<CallBase>(inst);
      const ThreadAPI::TD_TYPE type =
          call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;

      if (isNonBinarySemaphoreOp(m_thread_api, inst)) {
        continue;
      }

      std::vector<LockID> raii_releases = getRAIILocksReleasedAt(inst);
      for (LockID lock : raii_releases) {
        if (!lock) {
          continue;
        }
        m_all_locks.insert(lock);
        m_lock_releases[lock].push_back(inst);
      }
      if (!raii_releases.empty()) {
        continue;
      }

      const bool is_acquire =
          m_thread_api->isTDAcquire(inst) || type == ThreadAPI::TD_SHARED_RDLOCK ||
          type == ThreadAPI::TD_SHARED_WRLOCK ||
          type == ThreadAPI::TD_LOCK_GUARD_CTOR ||
          type == ThreadAPI::TD_UNIQUE_LOCK_CTOR ||
          type == ThreadAPI::TD_SCOPED_LOCK_CTOR ||
          type == ThreadAPI::TD_SHARED_LOCK_CTOR ||
          type == ThreadAPI::TD_UNIQUE_LOCK_LOCK;
      const bool is_release =
          m_thread_api->isTDRelease(inst) || type == ThreadAPI::TD_SHARED_UNLOCK ||
          type == ThreadAPI::TD_LOCK_GUARD_DTOR ||
          type == ThreadAPI::TD_UNIQUE_LOCK_DTOR ||
          type == ThreadAPI::TD_SCOPED_LOCK_DTOR ||
          type == ThreadAPI::TD_SHARED_LOCK_DTOR ||
          type == ThreadAPI::TD_UNIQUE_LOCK_UNLOCK;

      if (is_acquire) {
        LockID lock = getLockValue(inst);
        if (!lock) {
          lock = getCppWrapperLockValue(inst);
        }
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_acquires[lock].push_back(inst);

          LockSet entry_locks = getMayLockSetAt(inst);
          for (const auto *held : entry_locks) {
            if (held == lock || mayAlias(held, lock)) {
              m_reentrant_locks.insert(getCanonicalLock(lock));
              break;
            }
          }

          // Check for try-lock
          if (m_thread_api->isTryLock(inst))
            m_lock_try_acquires[lock].push_back(inst);
        }
      } else if (is_release) {
        LockID lock = getLockValue(inst);
        if (!lock) {
          lock = getCppWrapperLockValue(inst);
        }
        if (lock) {
          m_all_locks.insert(lock);
          m_lock_releases[lock].push_back(inst);
        }
      }
    }
  };

  if (m_module) {
    for (Function &func : *m_module) {
      if (!func.isDeclaration()) {
        process_func(func);
      }
    }
  } else if (m_single_function) {
    process_func(*m_single_function);
  }
}

void LockSetAnalysis::trackLockOrdering() {
  // Track the order in which locks are acquired
  for (const auto &pair : m_may_locksets_entry) {
    const Instruction *inst = pair.first;
    const LockSet &locks_held = pair.second;
    auto exit_it = m_may_locksets_exit.find(inst);
    if (exit_it == m_may_locksets_exit.end()) {
      continue;
    }

    LockSet newly_acquired;
    std::set_difference(exit_it->second.begin(), exit_it->second.end(),
                        locks_held.begin(), locks_held.end(),
                        std::inserter(newly_acquired, newly_acquired.begin()));

    auto markIfReentrantAcquire = [&](LockID acquired_lock) {
      if (!acquired_lock) {
        return;
      }
      for (const auto *held_lock : locks_held) {
        if (held_lock == acquired_lock || mayAlias(held_lock, acquired_lock)) {
          m_reentrant_locks.insert(getCanonicalLock(acquired_lock));
          return;
        }
      }
    };

    if (isLockOperation(inst)) {
      if (LockID op_lock = getLockValue(inst)) {
        const auto *call = dyn_cast<CallBase>(inst);
        const ThreadAPI::TD_TYPE type =
            call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
        if (type != ThreadAPI::TD_SHARED_RDLOCK &&
            type != ThreadAPI::TD_SHARED_WRLOCK &&
            type != ThreadAPI::TD_SHARED_LOCK_CTOR &&
            type != ThreadAPI::TD_SHARED_UNLOCK) {
          markIfReentrantAcquire(op_lock);
        }
      }
    }

    for (LockID new_lock : newly_acquired) {
      markIfReentrantAcquire(new_lock);

      for (const auto *held_lock : locks_held) {
        if (held_lock != new_lock) {
          m_observed_lock_orders.insert({held_lock, new_lock});
        }
      }
    }
  }
}

bool LockSetAnalysis::mayAlias(LockID lock1, LockID lock2) const {
  const Module *module =
      m_module ? m_module
               : (m_single_function ? m_single_function->getParent() : nullptr);
  if (areDisjointConstantOffsetPointers(lock1, lock2, module)) {
    return false;
  }

  lock1 = getCanonicalLock(lock1);
  lock2 = getCanonicalLock(lock2);
  if (lock1 == lock2)
    return true;

  // Use alias analysis wrapper if available
  if (m_alias_analysis && lock1 && lock2) {
    return m_alias_analysis->mayAlias(lock1, lock2);
  }

  // Conservative: assume may alias if no analysis available
  return true;
}

LockID LockSetAnalysis::getCanonicalLock(LockID lock) const {
  if (!lock)
    return nullptr;

  // Strip pointer casts first.
  lock = lock->stripPointerCasts();

  // Preserve constant-offset subobjects so distinct lock fields are not
  // collapsed to the same aggregate base.
  if (const auto *LI = dyn_cast<LoadInst>(lock)) {
    const Value *addr = LI->getPointerOperand()->stripPointerCasts();
    if (const auto *GEP = dyn_cast<GetElementPtrInst>(addr)) {
      if (GEP->hasAllConstantIndices()) {
        return addr;
      }
      lock = GEP->getPointerOperand()->stripPointerCasts();
    } else if (const Value *base = getUnderlyingObject(addr, 32)) {
      lock = base->stripPointerCasts();
    } else {
      lock = addr;
    }
  }

  if (const auto *GEP = dyn_cast<GetElementPtrInst>(lock)) {
    if (GEP->hasAllConstantIndices()) {
      return lock;
    }
    lock = GEP->getPointerOperand()->stripPointerCasts();
  } else if (const Value *base = getUnderlyingObject(lock, 32)) {
    lock = base->stripPointerCasts();
  }

  // If points-to resolves to a unique target, use the target object as the
  // canonical lock identity. This helps match locks loaded through wrapper
  // fields (e.g., w->mutex) across different SSA values.
  if (m_alias_analysis) {
    std::vector<const Value *> pts;
    if (m_alias_analysis->getPointsToSet(lock, pts) && pts.size() == 1 &&
        pts.front()) {
      return pts.front()->stripPointerCasts();
    }
  }

  return lock;
}

LockID LockSetAnalysis::getUnderlyingRAIILock(const Instruction *inst,
                                              const Value *lock_obj) const {
  std::vector<LockID> locks = getUnderlyingRAIILocks(inst, lock_obj);
  return locks.empty() ? nullptr : locks.front();
}

std::vector<LockID>
LockSetAnalysis::getUnderlyingRAIILocks(const Instruction *inst,
                                        const Value *lock_obj) const {
  std::vector<LockID> locks;
  if (!inst || !lock_obj) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  const Value *base = lock_obj->stripPointerCasts();
  if (const Value *underlying = getUnderlyingObject(base, 32)) {
    base = underlying->stripPointerCasts();
  }

  const auto *alloca = dyn_cast<AllocaInst>(base);
  if (!alloca) {
    return locks;
  }

  auto lifetime_it = raii_it->second.find(alloca);
  if (lifetime_it == raii_it->second.end()) {
    return locks;
  }

  for (const Value *lock : lifetime_it->second.underlyingLocks) {
    if (LockID canonical = getCanonicalLock(lock)) {
      locks.push_back(canonical);
    }
  }
  return locks;
}

std::vector<LockID>
LockSetAnalysis::getRAIILocksReleasedAt(const Instruction *inst) const {
  std::vector<LockID> locks;
  if (!inst) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (std::find(lifetime.destructors.begin(), lifetime.destructors.end(),
                  inst) == lifetime.destructors.end()) {
      continue;
    }

    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

std::vector<LockID>
LockSetAnalysis::getImpreciseRAIILocksEndingAt(const Instruction *inst) const {
  std::vector<LockID> locks;
  if (!inst) {
    return locks;
  }

  const Function *parent_func = inst->getFunction();
  auto raii_it = m_raii_locks.find(parent_func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (lifetime.hasPreciseLifetimeEnd) {
      continue;
    }
    const bool at_unknown_boundary =
        lifetime.impreciseLifetimeBoundary &&
        lifetime.impreciseLifetimeBoundary == inst;
    const bool at_function_exit_without_precise_lifetime =
        !lifetime.impreciseLifetimeBoundary &&
        (isa<ReturnInst>(inst) || isa<ResumeInst>(inst));
    if (!at_unknown_boundary && !at_function_exit_without_precise_lifetime) {
      continue;
    }
    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

std::vector<LockID>
LockSetAnalysis::getImpreciseRAIILocksInFunction(const Function *func) const {
  std::vector<LockID> locks;
  if (!func) {
    return locks;
  }

  auto raii_it = m_raii_locks.find(func);
  if (raii_it == m_raii_locks.end()) {
    return locks;
  }

  for (const auto &entry : raii_it->second) {
    const RAIILock::LockLifetime &lifetime = entry.second;
    if (lifetime.hasPreciseLifetimeEnd || !lifetime.impreciseLifetimeBoundary) {
      continue;
    }
    for (const Value *lock : lifetime.underlyingLocks) {
      if (LockID canonical = getCanonicalLock(lock)) {
        locks.push_back(canonical);
      }
    }
  }

  return locks;
}

LockID LockSetAnalysis::getCppWrapperLockValue(const Instruction *inst) const {
  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return nullptr;
  }

  switch (m_thread_api->getType(call)) {
  case ThreadAPI::TD_SHARED_RDLOCK:
  case ThreadAPI::TD_SHARED_WRLOCK:
  case ThreadAPI::TD_SHARED_UNLOCK:
    if (call->arg_size() >= 1) {
      return getCanonicalLock(call->getArgOperand(0));
    }
    return nullptr;

  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR:
    if (call->arg_size() >= 1) {
      if (LockID tracked =
              getUnderlyingRAIILock(inst, call->getArgOperand(0))) {
        return tracked;
      }
    }
    if (call->arg_size() >= 2) {
      return getCanonicalLock(call->getArgOperand(1));
    }
    return nullptr;

  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
    if (call->arg_size() >= 1) {
      return getUnderlyingRAIILock(inst, call->getArgOperand(0));
    }
    return nullptr;

  default:
    return nullptr;
  }
}

bool LockSetAnalysis::isLockOperation(const Instruction *inst) const {
  if (isNonBinarySemaphoreOp(m_thread_api, inst)) {
    return false;
  }
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    return true;
  }

  const auto *call = dyn_cast<CallBase>(inst);
  if (!call) {
    return false;
  }

  switch (m_thread_api->getType(call)) {
  case ThreadAPI::TD_SHARED_RDLOCK:
  case ThreadAPI::TD_SHARED_WRLOCK:
  case ThreadAPI::TD_SHARED_UNLOCK:
  case ThreadAPI::TD_LOCK_GUARD_CTOR:
  case ThreadAPI::TD_LOCK_GUARD_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
  case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
  case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
  case ThreadAPI::TD_SCOPED_LOCK_CTOR:
  case ThreadAPI::TD_SCOPED_LOCK_DTOR:
  case ThreadAPI::TD_SHARED_LOCK_CTOR:
  case ThreadAPI::TD_SHARED_LOCK_DTOR:
    return true;
  default:
    return false;
  }
}

LockID LockSetAnalysis::getLockValue(const Instruction *inst) const {
  if (m_thread_api->isTDAcquire(inst) || m_thread_api->isTDRelease(inst)) {
    const auto *call = dyn_cast<CallBase>(inst);
    ThreadAPI::TD_TYPE type =
        call ? m_thread_api->getType(call) : ThreadAPI::TD_DUMMY;
    switch (type) {
    case ThreadAPI::TD_LOCK_GUARD_CTOR:
    case ThreadAPI::TD_LOCK_GUARD_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_CTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_DTOR:
    case ThreadAPI::TD_UNIQUE_LOCK_LOCK:
    case ThreadAPI::TD_UNIQUE_LOCK_UNLOCK:
    case ThreadAPI::TD_SCOPED_LOCK_CTOR:
    case ThreadAPI::TD_SCOPED_LOCK_DTOR:
    case ThreadAPI::TD_SHARED_LOCK_CTOR:
    case ThreadAPI::TD_SHARED_LOCK_DTOR:
      return getCanonicalLock(m_thread_api->getLockVal(inst));
    default:
      if (const Value *identity = m_thread_api->getAnalysisLockIdentity(inst)) {
        return getCanonicalLock(identity);
      }
      return getCanonicalLock(m_thread_api->getLockVal(inst));
    }
  }
  return getCppWrapperLockValue(inst);
}

// ============================================================================
// Interprocedural Analysis Implementation
// ============================================================================

std::set<Function *> LockSetAnalysis::getCallees(const CallBase *call) const {
  std::set<Function *> callees;

  if (!call) {
    return callees;
  }

  // Try direct call first
  if (Function *direct_callee = call->getCalledFunction()) {
    if (!direct_callee->isDeclaration()) {
      callees.insert(direct_callee);
    }
    return callees;
  }

  if (const Value *called = call->getCalledOperand()) {
    if (const Function *direct_target =
            dyn_cast<Function>(called->stripPointerCasts())) {
      if (!direct_target->isDeclaration()) {
        callees.insert(const_cast<Function *>(direct_target));
      }
      return callees;
    }

    std::unordered_set<const Value *> visited_values;
    collectDefinedFunctionTargets(called, callees, visited_values);
  }

  // For indirect calls, use call graph if available
  if (m_call_graph) {
    Function *caller = const_cast<Function *>(call->getFunction());
    if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
      for (auto &callRecord : *cgNode) {
        if (!callRecord.first.hasValue() ||
            dyn_cast_or_null<CallBase>(*callRecord.first) != call) {
          continue;
        }
        if (Function *callee = callRecord.second->getFunction()) {
          if (!callee->isDeclaration()) {
            callees.insert(callee);
          }
        }
      }
    }
  }

  return callees;
}

bool LockSetAnalysis::shouldInvalidateMustLockState(
    const CallBase *call) const {
  if (!call) {
    return false;
  }

  const Function *direct = call->getCalledFunction();
  if (direct && direct->isIntrinsic()) {
    return false;
  }

  if (call->doesNotAccessMemory() || call->onlyReadsMemory()) {
    return false;
  }

  // If any potential target remains unresolved, keep may-locks unchanged but
  // drop must-lock certainty to avoid suppressing races after hidden unlocks or
  // helper-mediated lock state changes.
  return getCallees(call).empty() || hasUnresolvedCalleeTarget(call);
}

bool LockSetAnalysis::hasUnresolvedCalleeTarget(const CallBase *call) const {
  if (!call) {
    return false;
  }

  if (Function *direct = call->getCalledFunction()) {
    return direct->isDeclaration() && !direct->isIntrinsic();
  }

  if (const Value *called = call->getCalledOperand()) {
    if (const auto *direct_target =
            dyn_cast<Function>(called->stripPointerCasts())) {
      return direct_target->isDeclaration() && !direct_target->isIntrinsic();
    }
  }

  if (!m_call_graph) {
    return true;
  }

  Function *caller = const_cast<Function *>(call->getFunction());
  if (CallGraphNode *cgNode = (*m_call_graph)[caller]) {
    bool matched_record = false;
    for (auto &callRecord : *cgNode) {
      if (!callRecord.first.hasValue() ||
          dyn_cast_or_null<CallBase>(*callRecord.first) != call) {
        continue;
      }
      matched_record = true;
      CallGraphNode *callee_node = callRecord.second;
      if (!callee_node) {
        return true;
      }
      Function *callee = callee_node->getFunction();
      if (!callee || (callee->isDeclaration() && !callee->isIntrinsic())) {
        return true;
      }
    }
    return !matched_record;
  }

  return true;
}

void LockSetAnalysis::computeFunctionSummary(Function *func) {
  if (!func || func->isDeclaration()) {
    return;
  }

  auto &summary = m_function_summaries[func];
  if (summary.is_analyzed) {
    return;
  }

  errs() << "Computing summary for function: " << func->getName() << "\n";

  // Computes a summary of lock behaviors for the function to enable
  // interprocedural analysis.
  // - MayAcquire: Locks that *may* be acquired and held upon return.
  // - MustAcquire: Locks that *must* be acquired and held upon return.
  // - Releases: Locks released within the function.
  // This summary allows callers to update their locksets without re-analyzing
  // the callee inline.

  // Run intraprocedural analysis to get flow-sensitive results
  computeIntraproceduralLockSets(func);

  summary.may_acquire_delta.clear();
  summary.may_read_acquire_delta.clear();
  summary.may_write_acquire_delta.clear();
  summary.must_acquire_delta.clear();
  summary.must_read_acquire_delta.clear();
  summary.must_write_acquire_delta.clear();
  summary.may_release_delta.clear();
  summary.must_release_delta.clear();

  auto matchesLock = [this](LockID lhs, LockID rhs) {
    const LockID clhs = getCanonicalLock(lhs);
    const LockID crhs = getCanonicalLock(rhs);
    if (clhs && crhs && clhs == crhs) {
      return true;
    }
    return m_alias_analysis && clhs && crhs &&
           m_alias_analysis->mustAlias(clhs, crhs);
  };

  auto isDefinitelyHeldAt = [&](const Instruction *inst, LockID lock) {
    auto it = m_must_locksets_entry.find(inst);
    if (it == m_must_locksets_entry.end()) {
      return false;
    }
    for (LockID held : it->second) {
      if (matchesLock(held, lock)) {
        return true;
      }
    }
    return false;
  };

  auto isPossiblyHeldAt = [&](const Instruction *inst, LockID lock) {
    auto it = m_may_locksets_entry.find(inst);
    if (it == m_may_locksets_entry.end()) {
      return false;
    }
    for (LockID held : it->second) {
      if (matchesLock(held, lock)) {
        return true;
      }
    }
    return false;
  };

  auto isBinarySemaphoreOnlyLockInFunction = [&](LockID lock) {
    bool saw_binary_semaphore = false;
    for (const Instruction &inst : instructions(*func)) {
      if (!m_thread_api->isTDAcquire(&inst)) {
        continue;
      }
      LockID inst_lock = getLockValue(&inst);
      if (!inst_lock || !matchesLock(inst_lock, lock)) {
        continue;
      }
      if (!m_thread_api->isBinarySemaphoreOp(&inst)) {
        return false;
      }
      saw_binary_semaphore = true;
    }
    return saw_binary_semaphore;
  };
  DominatorTree dom(*func);

  auto intersectMustSets = [&](const LockSet &lhs, const LockSet &rhs) {
    std::vector<LockSet> inputs;
    inputs.push_back(lhs);
    inputs.push_back(rhs);
    return merge(inputs, true);
  };

  // Collect return instructions
  std::vector<const ReturnInst *> returns;
  for (const BasicBlock &bb : *func) {
    if (const ReturnInst *ret = dyn_cast<ReturnInst>(bb.getTerminator())) {
      returns.push_back(ret);
    }
  }

  auto returnMayObserveUnmatchedRelease =
      [&](const ReturnInst *ret,
          const std::vector<const Instruction *> &release_sites) {
        if (!ret) {
          return false;
        }
        auto blockCanReach = [](const BasicBlock *from, const BasicBlock *to) {
          if (!from || !to) {
            return false;
          }
          if (from == to) {
            return true;
          }
          std::queue<const BasicBlock *> worklist;
          std::set<const BasicBlock *> visited;
          worklist.push(from);
          visited.insert(from);
          while (!worklist.empty()) {
            const BasicBlock *current = worklist.front();
            worklist.pop();
            for (const BasicBlock *succ : successors(current)) {
              if (!visited.insert(succ).second) {
                continue;
              }
              if (succ == to) {
                return true;
              }
              worklist.push(succ);
            }
          }
          return false;
        };
        for (const Instruction *release_inst : release_sites) {
          if (!release_inst || release_inst->getFunction() != func) {
            continue;
          }
          if (release_inst->getParent() == ret->getParent()) {
            if (release_inst->comesBefore(ret)) {
              return true;
            }
            continue;
          }
          if (blockCanReach(release_inst->getParent(), ret->getParent())) {
            return true;
          }
        }
        return false;
      };

  auto returnMustObserveUnmatchedRelease =
      [&](const ReturnInst *ret,
          const std::vector<const Instruction *> &release_sites) {
        if (!ret) {
          return false;
        }
        for (const Instruction *release_inst : release_sites) {
          if (!release_inst || release_inst->getFunction() != func) {
            continue;
          }
          if (release_inst->getParent() == ret->getParent()) {
            if (release_inst->comesBefore(ret)) {
              return true;
            }
            continue;
          }
          if (dom.dominates(release_inst->getParent(), ret->getParent())) {
            return true;
          }
        }
        return false;
      };

  if (!returns.empty()) {
    // Preserve read-vs-write mode so callers do not accidentally turn a held
    // shared lock into an exclusive one.
    bool seeded_must_read = false;
    bool seeded_must_write = false;
    LockSet must_read_intersection;
    LockSet must_write_intersection;
    for (const auto *ret : returns) {
      auto it_read = m_may_read_locks_exit.find(ret);
      if (it_read != m_may_read_locks_exit.end()) {
        for (LockID lock : it_read->second) {
          if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
            summary.may_read_acquire_delta.insert(lock);
          }
        }
      }

      auto it_write = m_may_write_locks_exit.find(ret);
      if (it_write != m_may_write_locks_exit.end()) {
        for (LockID lock : it_write->second) {
          if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
            summary.may_write_acquire_delta.insert(lock);
          }
        }
      }

      auto it_must_read = m_must_read_locks_exit.find(ret);
      if (it_must_read != m_must_read_locks_exit.end()) {
        if (!seeded_must_read) {
          must_read_intersection = it_must_read->second;
          seeded_must_read = true;
        } else {
          must_read_intersection =
              intersectMustSets(must_read_intersection, it_must_read->second);
        }
      } else {
        must_read_intersection.clear();
        seeded_must_read = true;
      }

      auto it_must_write = m_must_write_locks_exit.find(ret);
      if (it_must_write != m_must_write_locks_exit.end()) {
        if (!seeded_must_write) {
          must_write_intersection = it_must_write->second;
          seeded_must_write = true;
        } else {
          must_write_intersection = intersectMustSets(
              must_write_intersection, it_must_write->second);
        }
      } else {
        must_write_intersection.clear();
        seeded_must_write = true;
      }
    }

    summary.may_acquire_delta = summary.may_read_acquire_delta;
    summary.may_acquire_delta.insert(summary.may_write_acquire_delta.begin(),
                                     summary.may_write_acquire_delta.end());

    if (seeded_must_read) {
      for (LockID lock : must_read_intersection) {
        if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
          summary.must_read_acquire_delta.insert(lock);
        }
      }
    }
    if (seeded_must_write) {
      for (LockID lock : must_write_intersection) {
        if (!isBinarySemaphoreOnlyLockInFunction(lock)) {
          summary.must_write_acquire_delta.insert(lock);
        }
      }
    }

    summary.must_acquire_delta = summary.must_read_acquire_delta;
    summary.must_acquire_delta.insert(summary.must_write_acquire_delta.begin(),
                                      summary.must_write_acquire_delta.end());
  }

  // Track releases that are caller-visible on some path versus all paths.
  // A release is:
  // - maybe caller-visible if the lock is not definitely held at the site
  //   (there exists a path where the callee did not acquire it internally),
  // - definitely caller-visible only if the lock is not even possibly held by
  //   an internal callee acquisition at that site.
  std::unordered_map<LockID, std::vector<const Instruction *>>
      maybe_unmatched_releases;
  std::unordered_map<LockID, std::vector<const Instruction *>>
      must_unmatched_releases;
  for (inst_iterator I = inst_begin(func), E = inst_end(func); I != E; ++I) {
    Instruction *inst = &*I;
    std::vector<LockID> released = getRAIILocksReleasedAt(inst);
    if (!released.empty()) {
      for (LockID lock : released) {
        if (!isDefinitelyHeldAt(inst, lock)) {
          maybe_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
        if (!isPossiblyHeldAt(inst, lock)) {
          must_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
      continue;
    }
    if (isNonBinarySemaphoreOp(m_thread_api, inst)) {
      continue;
    }
    if (m_thread_api->isTDRelease(inst) && !m_thread_api->isTDAcquire(inst)) {
      if (LockID lock = getLockValue(inst)) {
        if (!isDefinitelyHeldAt(inst, lock)) {
          maybe_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
        if (!isPossiblyHeldAt(inst, lock)) {
          must_unmatched_releases[getCanonicalLock(lock)].push_back(inst);
        }
      }
    }
  }

  for (const auto &entry : maybe_unmatched_releases) {
    LockID lock = entry.first;
    bool reaches_any_return = false;
    for (const ReturnInst *ret : returns) {
      reaches_any_return |= returnMayObserveUnmatchedRelease(ret, entry.second);
    }
    if (reaches_any_return) {
      summary.may_release_delta.insert(lock);
    }
  }

  for (const auto &entry : must_unmatched_releases) {
    LockID lock = entry.first;
    bool covers_all_returns = !returns.empty();
    for (const ReturnInst *ret : returns) {
      covers_all_returns &=
          returnMustObserveUnmatchedRelease(ret, entry.second);
    }
    if (covers_all_returns) {
      summary.must_release_delta.insert(lock);
    }
  }

  for (LockID lock : getImpreciseRAIILocksInFunction(func)) {
    summary.may_acquire_delta.erase(lock);
    summary.may_read_acquire_delta.erase(lock);
    summary.may_write_acquire_delta.erase(lock);
    summary.must_acquire_delta.erase(lock);
    summary.must_read_acquire_delta.erase(lock);
    summary.must_write_acquire_delta.erase(lock);
  }

  summary.is_analyzed = true;

  errs() << "  May acquire delta: " << summary.may_acquire_delta.size()
         << " locks\n";
  errs() << "  Must acquire delta: " << summary.must_acquire_delta.size()
         << " locks\n";
  errs() << "  May release delta: " << summary.may_release_delta.size()
         << " locks\n";
  errs() << "  Must release delta: " << summary.must_release_delta.size()
         << " locks\n";
}

void LockSetAnalysis::applyFunctionSummary(const CallBase *call,
                                           const Function *callee,
                                           LockSet &may_locks,
                                           LockSet &must_locks) const {
  if (!call || !callee) {
    return;
  }

  auto it = m_function_summaries.find(callee);
  if (it == m_function_summaries.end() || !it->second.is_analyzed) {
    return;
  }

  const FunctionSummary &summary = it->second;

  // Apply lock acquisitions
  may_locks.insert(summary.may_acquire_delta.begin(),
                   summary.may_acquire_delta.end());
  must_locks.insert(summary.must_acquire_delta.begin(),
                    summary.must_acquire_delta.end());

  // Apply lock releases (remove from locksets)
  for (LockID lock : summary.may_release_delta) {
    must_locks.erase(lock);

    // Also remove aliases if alias analysis is available
    if (m_alias_analysis) {
      LockSet to_remove;
      to_remove.clear();
      for (const auto *l : must_locks) {
        if (mayAlias(l, lock)) {
          to_remove.insert(l);
        }
      }
      for (const auto *l : to_remove) {
        must_locks.erase(l);
      }
    }
  }

  for (LockID lock : summary.must_release_delta) {
    may_locks.erase(lock);
    must_locks.erase(lock);

    // Also remove aliases if alias analysis is available
    if (m_alias_analysis) {
      LockSet to_remove;
      for (const auto *l : must_locks) {
        if (mayAlias(l, lock)) {
          to_remove.insert(l);
        }
      }
      for (const auto *l : to_remove) {
        must_locks.erase(l);
      }
    }
  }
}

void LockSetAnalysis::bottomUpTraversal() {
  if (!m_call_graph) {
    return;
  }

  errs() << "Performing bottom-up call graph traversal...\n";

  // Compute post-order traversal for bottom-up analysis using LLVM CallGraph
  std::vector<Function *> post_order;
  std::set<Function *> visited;
  std::stack<std::pair<Function *, bool>> stack;

  // Start from all functions in the module
  for (Function &func : *m_module) {
    if (!func.isDeclaration()) {
      if (visited.find(&func) == visited.end()) {
        stack.push({&func, false});

        while (!stack.empty()) {
          auto top_pair = stack.top();
          stack.pop();
          Function *current = top_pair.first;
          bool children_visited = top_pair.second;

          if (children_visited) {
            post_order.push_back(current);
            continue;
          }

          if (visited.find(current) != visited.end()) {
            continue;
          }

          visited.insert(current);
          stack.push({current, true});

          // Get callees from LLVM CallGraph
          if (CallGraphNode *cgNode = (*m_call_graph)[current]) {
            for (auto &callRecord : *cgNode) {
              if (Function *callee = callRecord.second->getFunction()) {
                if (callee && !callee->isDeclaration() &&
                    visited.find(callee) == visited.end()) {
                  stack.push({callee, false});
                }
              }
            }
          }
        }
      }
    }
  }

  errs() << "Processing " << post_order.size()
         << " functions in bottom-up order\n";

  // Compute summaries in post-order (callees before callers)
  for (Function *func : post_order) {
    computeFunctionSummary(func);
  }

  errs() << "Bottom-up traversal complete\n";
}
