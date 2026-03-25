/**
 * @file LockSetAnalysis.h
 * @brief Production-ready Lock Set Analysis for Multithreaded Programs
 *
 * This file provides a comprehensive lock set analysis that computes the
 * sets of locks that may or must be held at each program point. This is
 * essential for:
 * - Data race detection
 * - Deadlock detection
 * - MHP (May-Happen-in-Parallel) analysis
 * - Lock ordering verification
 *
 * Key Features:
 * - Intraprocedural lock set computation
 * - Interprocedural lock set propagation
 * - May-lockset analysis (over-approximation)
 * - Must-lockset analysis (under-approximation)
 * - Lock aliasing support
 * - Reentrant lock handling
 * - Support for try-lock operations
 *
 * @author rainoftime
 * @date 2026
 */

#ifndef LOCKSET_ANALYSIS_H
#define LOCKSET_ANALYSIS_H

#include "Analysis/Concurrency/Utils/RAIILockTracker.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace llvm {
class CallGraph;
} // namespace llvm

namespace mhp {

// ============================================================================
// Type Definitions
// ============================================================================

using LockID = const llvm::Value *;
using LockSet = std::set<LockID>;

// ============================================================================
// Lock Set Analysis
// ============================================================================

/**
 * @brief Comprehensive lock set analysis for concurrent programs
 *
 * Computes may-locksets and must-locksets at each program point using
 * dataflow analysis. Handles:
 * - pthread_mutex_lock/unlock
 * - pthread_rwlock operations
 * - binary semaphores only when explicitly tagged as exclusion-capable
 * - Reentrant locks
 * - Try-lock operations
 *
 * Usage:
 *   LockSetAnalysis lsa(module);
 *   lsa.analyze();
 *   LockSet locks = lsa.getMayLockSetAt(inst);
 */
class LockSetAnalysis {
public:
  // ========================================================================
  // Construction and Analysis
  // ========================================================================

  /**
   * @brief Construct lock set analysis for a module
   * @param module The LLVM module to analyze
   */
  explicit LockSetAnalysis(llvm::Module &module);

  /**
   * @brief Construct lock set analysis for a single function
   * @param func The function to analyze
   */
  explicit LockSetAnalysis(llvm::Function &func);

  ~LockSetAnalysis() = default;

  /**
   * @brief Run the lock set analysis
   */
  void analyze();

  /**
   * @brief Set alias analysis wrapper for better precision
   * @param aa_wrapper Alias analysis wrapper instance
   */
  void setAliasAnalysis(lotus::AliasAnalysisWrapper *aa_wrapper) {
    m_alias_analysis = aa_wrapper;
  }

  /**
   * @brief Set call graph for interprocedural analysis
   * @param cg LLVM CallGraph instance
   */
  void setCallGraph(llvm::CallGraph *cg) { m_call_graph = cg; }

  // ========================================================================
  // Query Interface
  // ========================================================================

  /**
   * @brief Get locks that may be held at instruction (union of read + write)
   * @param inst Target instruction
   * @return Set of locks that may be held
   */
  LockSet getMayLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get read locks that may be held (rwlock_rdlock)
   */
  LockSet getMayReadLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get write locks that may be held (mutex or rwlock_wrlock)
   */
  LockSet getMayWriteLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get locks that must be held at instruction
   */
  LockSet getMustLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get read locks that must be held
   */
  LockSet getMustReadLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Get write locks that must be held
   */
  LockSet getMustWriteLockSetAt(const llvm::Instruction *inst) const;

  /**
   * @brief Check if a lock may be held at instruction
   * @param inst Target instruction
   * @param lock Lock value
   * @return true if lock may be held
   */
  bool mayHoldLock(const llvm::Instruction *inst, LockID lock) const;

  /**
   * @brief Check if a lock must be held at instruction
   * @param inst Target instruction
   * @param lock Lock value
   * @return true if lock must be held
   */
  bool mustHoldLock(const llvm::Instruction *inst, LockID lock) const;

  /**
   * @brief Get all instructions that may hold a specific lock
   * @param lock Lock value
   * @return Set of instructions
   */
  std::unordered_set<const llvm::Instruction *>
  getInstructionsHoldingLock(LockID lock) const;

  /**
   * @brief Check if two instructions may hold a common lock (mutex or rwlock).
   * For rwlocks: true if both hold same lock in write mode, or both in read
   * mode.
   * @param i1 First instruction
   * @param i2 Second instruction
   * @return true if they may hold a common lock
   */
  bool mayHoldCommonLock(const llvm::Instruction *i1,
                         const llvm::Instruction *i2) const;

  bool mustHoldCommonLock(const llvm::Instruction *i1,
                          const llvm::Instruction *i2) const;

  /**
   * @brief Get all locks that may be held in a function
   * @param func Target function
   * @return Set of all locks
   */
  LockSet getAllLocksInFunction(const llvm::Function *func) const;

  /**
   * @brief Get lock acquire instructions for a specific lock
   * @param lock Lock value
   * @return Vector of acquire instructions
   */
  std::vector<const llvm::Instruction *> getLockAcquires(LockID lock) const;

  /**
   * @brief Get lock release instructions for a specific lock
   * @param lock Lock value
   * @return Vector of release instructions
   */
  std::vector<const llvm::Instruction *> getLockReleases(LockID lock) const;

  // ========================================================================
  // Advanced Queries
  // ========================================================================

  /**
   * @brief Check if a lock is reentrant (acquired multiple times in same path)
   * @param lock Lock value
   * @return true if lock may be reentrant
   */
  bool isReentrantLock(LockID lock) const;

  /**
   * @brief Get the lock nesting depth at an instruction
   * @param inst Target instruction
   * @return Maximum nesting depth (0 if no locks held)
   */
  size_t getLockNestingDepth(const llvm::Instruction *inst) const;

  /**
   * @brief Check if locks are acquired in consistent order
   * @param lock1 First lock
   * @param lock2 Second lock
   * @return true if order is consistent (no potential deadlock)
   */
  bool areLocksOrderedConsistently(LockID lock1, LockID lock2) const;

  /**
   * @brief Detect potential lock order inversions (deadlock candidates)
   * @return Pairs of locks that may cause deadlock
   */
  std::vector<std::pair<LockID, LockID>> detectLockOrderInversions() const;

  // ========================================================================
  // Statistics and Debugging
  // ========================================================================

  struct Statistics {
    size_t num_locks;               ///< Total number of distinct locks
    size_t num_acquires;            ///< Total lock acquire operations
    size_t num_releases;            ///< Total lock release operations
    size_t num_try_acquires;        ///< Total try-lock operations
    size_t max_nesting_depth;       ///< Maximum observed lock nesting
    size_t num_reentrant_locks;     ///< Number of reentrant locks
    size_t num_potential_deadlocks; ///< Number of potential deadlocks

    void print(llvm::raw_ostream &os) const;
  };

  Statistics getStatistics() const;
  void printStatistics(llvm::raw_ostream &os) const;
  void printResults(llvm::raw_ostream &os) const;
  void printLockSetsForFunction(const llvm::Function *func,
                                llvm::raw_ostream &os) const;

  // ========================================================================
  // Visualization
  // ========================================================================

  /**
   * @brief Dump lock acquisition graph as DOT format
   * @param filename Output file name
   */
  void dumpLockGraph(const std::string &filename) const;

  /**
   * @brief Print lock sets in a readable format
   * @param os Output stream
   */
  void print(llvm::raw_ostream &os) const;

private:
  // ========================================================================
  // Data Structures
  // ========================================================================

  llvm::Module *m_module;
  llvm::Function *m_single_function; // For single-function analysis
  ThreadAPI *m_thread_api;
  lotus::AliasAnalysisWrapper *m_alias_analysis;
  llvm::CallGraph *m_call_graph; // For interprocedural analysis
  std::unique_ptr<llvm::CallGraph> m_owned_call_graph;

  // Lockset results (combined for backward compat; read/write for rwlock)
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_locksets_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_locksets_exit;
  std::unordered_map<const llvm::Instruction *, LockSet> m_must_locksets_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_must_locksets_exit;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_read_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_read_locks_exit;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_may_write_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_may_write_locks_exit;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_must_read_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet> m_must_read_locks_exit;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_must_write_locks_entry;
  std::unordered_map<const llvm::Instruction *, LockSet>
      m_must_write_locks_exit;

  // Lock tracking
  std::unordered_set<LockID> m_all_locks;
  std::unordered_map<LockID, std::vector<const llvm::Instruction *>>
      m_lock_acquires;
  std::unordered_map<LockID, std::vector<const llvm::Instruction *>>
      m_lock_releases;
  std::unordered_map<LockID, std::vector<const llvm::Instruction *>>
      m_lock_try_acquires;

  // Lock ordering tracking
  struct LockPair {
    LockID first;
    LockID second;

    bool operator==(const LockPair &other) const {
      return first == other.first && second == other.second;
    }

    struct Hash {
      size_t operator()(const LockPair &p) const {
        return std::hash<LockID>()(p.first) ^ std::hash<LockID>()(p.second);
      }
    };
  };

  std::unordered_set<LockPair, LockPair::Hash> m_observed_lock_orders;
  std::unordered_set<LockID> m_reentrant_locks;

  // RAII lock tracking per function (type alias avoids C++11 >> parse issue)
  typedef std::map<const llvm::Value *, RAIILock::LockLifetime>
      RAIILockMap;
  std::unordered_map<const llvm::Function *, RAIILockMap> m_raii_locks;

  // Interprocedural analysis data structures
  struct FunctionSummary {
    LockSet may_acquire_delta;  ///< Locks newly held on some exit path
    LockSet may_read_acquire_delta;  ///< Read locks newly held on some exit path
    LockSet may_write_acquire_delta; ///< Write locks newly held on some exit path
    LockSet must_acquire_delta; ///< Locks newly held on all normal exit paths
    LockSet must_read_acquire_delta;  ///< Read locks newly held on all exits
    LockSet must_write_acquire_delta; ///< Write locks newly held on all exits
    LockSet may_release_delta;  ///< Locks maybe released on some returning path
    LockSet must_release_delta; ///< Locks definitely released on all returning paths
    bool is_analyzed;     ///< Whether function has been analyzed

    FunctionSummary() : is_analyzed(false) {}
  };

  std::unordered_map<const llvm::Function *, FunctionSummary>
      m_function_summaries;

  // ========================================================================
  // Analysis Implementation
  // ========================================================================

  /**
   * @brief Analyze a single function
   */
  void analyzeFunction(llvm::Function *func);

  /**
   * @brief Perform intraprocedural dataflow analysis
   */
  void computeIntraproceduralLockSets(llvm::Function *func);

  /**
   * @brief Perform interprocedural analysis
   */
  void computeInterproceduralLockSets();

  /**
   * @brief Transfer function for lockset analysis (combined set for backward
   * compat)
   */
  LockSet transfer(const llvm::Instruction *inst, const LockSet &in_set,
                   bool is_must) const;

  /**
   * @brief Transfer for read/write locks (rdlock, wrlock, unlock)
   */
  void transferReadWrite(const llvm::Instruction *inst, const LockSet &in_read,
                         const LockSet &in_write, LockSet &out_read,
                         LockSet &out_write, bool is_must) const;

  /**
   * @brief Merge locksets from multiple predecessors
   * @param sets Vector of locksets to merge
   * @param is_must True for must-analysis (intersection), false for may (union)
   * @return Merged lockset
   */
  LockSet merge(const std::vector<LockSet> &sets, bool is_must) const;

  /**
   * @brief Identify all locks in the program
   */
  void identifyLocks();

  /**
   * @brief Track lock ordering relationships
   */
  void trackLockOrdering();

  /**
   * @brief Check if two locks may alias
   */
  bool mayAlias(LockID lock1, LockID lock2) const;

  /**
   * @brief Get canonical lock value (handling aliases)
   */
  LockID getCanonicalLock(LockID lock) const;

  /**
   * @brief Resolve the underlying mutex for a RAII lock object when known
   */
  LockID getUnderlyingRAIILock(const llvm::Instruction *inst,
                               const llvm::Value *lock_obj) const;
  std::vector<LockID> getUnderlyingRAIILocks(const llvm::Instruction *inst,
                                             const llvm::Value *lock_obj) const;
  std::vector<LockID>
  getRAIILocksReleasedAt(const llvm::Instruction *inst) const;
  std::vector<LockID>
  getImpreciseRAIILocksEndingAt(const llvm::Instruction *inst) const;
  std::vector<LockID>
  getImpreciseRAIILocksInFunction(const llvm::Function *func) const;

  /**
   * @brief Resolve the lock identity for explicit C++ wrapper operations
   */
  LockID getCppWrapperLockValue(const llvm::Instruction *inst) const;

  /**
   * @brief Check if instruction is a lock operation
   */
  bool isLockOperation(const llvm::Instruction *inst) const;

  /**
   * @brief Get lock value from lock operation
   */
  LockID getLockValue(const llvm::Instruction *inst) const;

  /**
   * @brief Compute function summary for interprocedural analysis
   */
  void computeFunctionSummary(llvm::Function *func);

  /**
   * @brief Apply function summary at call site
   */
  void applyFunctionSummary(const llvm::CallBase *call,
                            const llvm::Function *callee, LockSet &may_locks,
                            LockSet &must_locks) const;

  /**
   * @brief Get callees at a call site using CallGraph
   */
  std::set<llvm::Function *> getCallees(const llvm::CallBase *call) const;
  bool hasUnresolvedCalleeTarget(const llvm::CallBase *call) const;
  bool shouldInvalidateMustLockState(const llvm::CallBase *call) const;

  /**
   * @brief Perform bottom-up call graph traversal for interprocedural analysis
   */
  void bottomUpTraversal();
};

} // namespace mhp

#endif // LOCKSET_ANALYSIS_H
