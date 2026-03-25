/**
 * @file StaticVectorClockMHP.h
 * @brief Static vector clock based MHP analysis (SVC-MHP)
 *
 * This analysis implements the static vector clock algorithm described in
 * "May-Happen-in-Parallel Analysis with Static Vector Clocks" (CGO'18).
 * It reuses the thread-flow graph builder and constructs context-sensitive
 * static threads (keyed by fork-site contexts). It then computes static vector
 * clocks following the transfer rules in the paper to answer MHP/HB queries.
 */

#ifndef STATIC_VECTOR_CLOCK_MHP_H
#define STATIC_VECTOR_CLOCK_MHP_H

#include "Analysis/Concurrency/JoinTarget/JoinTargetAnalysis.h"
#include "Analysis/Concurrency/MHP/IMHPAnalysis.h"
#include "Analysis/Concurrency/Utils/ThreadAPI.h"
#include "Analysis/Concurrency/Utils/ThreadFlowGraph.h"
#include "Analysis/Concurrency/Utils/ThreadMultiplicity.h"

#include <limits>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace mhp {

using StaticThreadID = size_t;

/**
 * @brief Static Vector Clock MHP Analysis (SVC-MHP)
 *
 * The analysis builds on the existing ThreadFlowGraph constructed by
 * MHPAnalysis, computes static vector clocks for every synchronization
 * node, and answers MHP queries by comparing those clocks.
 *
 * Usage:
 *   StaticVectorClockMHP svc(module);
 *   svc.analyze();
 *   bool parallel = svc.mayHappenInParallel(instA, instB);
 *   svc.printResults(errs());
 */
class StaticVectorClockMHP : public IMHPAnalysis {
public:
  explicit StaticVectorClockMHP(llvm::Module &module);

  /// Run the SVC-MHP analysis.
  void analyze() override;

  /// Query whether two instructions may execute in parallel.
  bool mayHappenInParallel(const llvm::Instruction *i1,
                           const llvm::Instruction *i2) const override;

  bool isPrecomputedMHP(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const override;

  InstructionSet
  getParallelInstructions(const llvm::Instruction *inst) const override;

  bool mustBeSequential(const llvm::Instruction *i1,
                        const llvm::Instruction *i2) const override {
    return !mayHappenInParallel(i1, i2);
  }

  ThreadID getThreadID(const llvm::Instruction *inst) const override;

  InstructionSet getInstructionsInThread(ThreadID tid) const override;

  size_t getMhpPairCount() const override { return m_mhp_pairs.size(); }

  /// Query happens-before using static vector clocks.
  bool happensBefore(const llvm::Instruction *i1,
                     const llvm::Instruction *i2) const;

  /// Print a compact statistics summary.
  void printStatistics(llvm::raw_ostream &os) const override;

  /// Print debug information about the computed clocks and pairs.
  void printResults(llvm::raw_ostream &os) const override;

  const ThreadFlowGraph &getThreadFlowGraph() const { return *m_tfg; }

  static constexpr unsigned kCallContextLimit =
      2; // k-limiting for call strings

private:
  using CallString = std::vector<size_t>;

  struct CallStringHash {
    size_t operator()(const CallString &c) const {
      size_t h = 0;
      for (size_t v : c)
        h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2);
      return h;
    }
  };

  struct Context {
    std::vector<size_t> call_sites; // call-string from thread entry (k-limited)
    std::vector<size_t> fork_sites; // sequence of SyncNode IDs (fork sites)

    bool operator==(const Context &other) const {
      return call_sites == other.call_sites && fork_sites == other.fork_sites;
    }
  };

  struct ContextHash {
    size_t operator()(const Context &c) const {
      size_t h = 1469598103934665603ULL;
      for (size_t v : c.call_sites) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      }
      for (size_t v : c.fork_sites) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
      }
      return h;
    }
  };

  // Paper: LC = ⊤ | S | ⊥ | n@c (logic clocks)
  struct LogicClockElem {
    enum class Kind { Top, Start, Terminated, Node };
    Kind kind;
    size_t node_id; // valid when kind == Node

    bool operator==(const LogicClockElem &o) const {
      return kind == o.kind && node_id == o.node_id;
    }
  };

  struct LogicClockElemHash {
    size_t operator()(const LogicClockElem &e) const {
      return (static_cast<size_t>(e.kind) * 1315423911u) ^
             (e.node_id + 0x9e3779b9 + (e.node_id << 6) + (e.node_id >> 2));
    }
  };

  using LogicClockSet = std::unordered_set<LogicClockElem, LogicClockElemHash>;

  struct StaticVectorClock {
    std::unordered_map<StaticThreadID, LogicClockSet> entries;

    bool mergeFrom(const StaticVectorClock &other);
    bool leq(const StaticVectorClock &other) const;
  };

  struct StaticNodeKey {
    const SyncNode *node = nullptr;
    StaticThreadID stid = 0;
    CallString call_sites;

    bool operator==(const StaticNodeKey &other) const {
      return node == other.node && stid == other.stid &&
             call_sites == other.call_sites;
    }
  };

  struct StaticNodeKeyHash {
    size_t operator()(const StaticNodeKey &key) const {
      size_t seed = std::hash<const SyncNode *>{}(key.node);
      seed ^= std::hash<StaticThreadID>{}(key.stid) + 0x9e3779b9 + (seed << 6) +
              (seed >> 2);
      seed ^= CallStringHash{}(key.call_sites) + 0x9e3779b9 + (seed << 6) +
              (seed >> 2);
      return seed;
    }
  };

  using StaticNodeList = std::vector<StaticNodeKey>;

  // Paper partial order: ⊥ ≤ S ≤ n@c ≤ T. leq(LC_a, LC_b) and Max(LC, LC') for
  // sets.
  bool logicClockLeq(const LogicClockElem &a, const LogicClockElem &b,
                     StaticThreadID stid) const;
  void logicClockMax(const LogicClockSet &la, const LogicClockSet &lb,
                     StaticThreadID stid, LogicClockSet &out) const;
  bool nodeReachesInStaticThread(size_t from_node_id, size_t to_node_id,
                                 StaticThreadID stid) const;
  void computeReachabilityPerStaticThread();
  mutable std::unordered_map<
      StaticThreadID, std::unordered_map<size_t, std::unordered_set<size_t>>>
      m_reachable_node_ids;
  std::unordered_map<StaticNodeKey, size_t, StaticNodeKeyHash>
      m_static_node_to_id;
  std::vector<StaticNodeKey> m_static_node_by_id;

  // Return-site -> call-site mapping for context-sensitive Ret edge handling
  std::unordered_map<const SyncNode *, const SyncNode *> m_ret_to_call;

  struct StaticThread {
    StaticThreadID id;
    Context ctx;
    ThreadID base_tid;               // originating TFG thread
    const SyncNode *entry = nullptr; // entry node in this static thread
    std::vector<StaticNodeKey> nodes;
  };

  llvm::Module &m_module;

  // Thread-flow graph owned by this analysis
  ThreadAPI *m_thread_api = nullptr;
  std::unique_ptr<ThreadFlowGraph> m_tfg;
  std::unique_ptr<llvm::CallGraph> m_call_graph;
  std::unique_ptr<JoinTargetAnalysis> m_join_target_analysis;
  std::unique_ptr<concurrency::ThreadMultiplicityAnalysis>
      m_thread_multiplicity;

  // Static thread management
  std::unordered_map<Context, StaticThreadID, ContextHash> m_ctx_to_stid;
  std::vector<StaticThread> m_static_threads;

  std::unordered_map<const SyncNode *, StaticNodeList>
      m_sync_node_to_static_nodes;
  std::unordered_map<const llvm::Instruction *, StaticNodeList>
      m_inst_to_static_nodes;
  std::unordered_map<StaticNodeKey, StaticNodeList, StaticNodeKeyHash>
      m_static_node_predecessors;
  std::unordered_map<StaticNodeKey, StaticNodeList, StaticNodeKeyHash>
      m_static_node_successors;
  std::vector<StaticNodeKey> m_static_node_keys;

  // Static vector clocks per static-node instance
  std::unordered_map<StaticNodeKey, StaticVectorClock, StaticNodeKeyHash>
      m_static_node_clocks;

  std::set<std::pair<const llvm::Instruction *, const llvm::Instruction *>>
      m_mhp_pairs;
  mutable std::unordered_map<ThreadID, InstructionSet>
      m_thread_instruction_cache;
  mutable std::unordered_map<const llvm::Instruction *, InstructionSet>
      m_parallel_instruction_cache;

  // Construction
  void buildThreadFlowGraph();
  void processFunction(const llvm::Function *func, ThreadID tid,
                       CallContextID ctx = 0);
  void mapInstructionToThread(const llvm::Instruction *inst, ThreadID tid);
  ThreadID allocateThreadID();
  void handleThreadFork(const llvm::Instruction *fork_inst, SyncNode *node,
                        ThreadID parent_tid);
  void handleThreadJoin(const llvm::Instruction *join_inst, SyncNode *node,
                        ThreadID parent_tid);
  void handleThreadDetach(const llvm::Instruction *detach_inst);
  void handleLockAcquire(const llvm::Instruction *lock_inst, SyncNode *node);
  void handleLockRelease(const llvm::Instruction *unlock_inst, SyncNode *node);
  void handleCondWait(const llvm::Instruction *wait_inst, SyncNode *node);
  void handleCondSignal(const llvm::Instruction *signal_inst, SyncNode *node);
  void handleBarrier(const llvm::Instruction *barrier_inst, SyncNode *node);
  std::vector<SyncNode *>
  getBarrierContinuations(const llvm::Instruction *barrier_inst) const;
  void wireSynchronizationEdges();
  void buildStaticThreads();
  void buildStaticEdges();
  void initializeNodeClocks();
  StaticThreadID getOrCreateStaticThread(const Context &ctx, ThreadID base_tid,
                                         const SyncNode *entry);

  // Clock computation
  void computeStaticVectorClocks();
  StaticVectorClock initialClockFor(const StaticThread &st) const;
  bool transfer(const StaticNodeKey &key);
  StaticVectorClock
  mergePredecessorClocksWithRules(const StaticNodeKey &key) const;
  bool shouldAddEventAtNode(const StaticNodeKey &key) const;
  StaticVectorClock mergePredecessorClocks(const StaticNodeKey &key) const;
  void addEventToClock(const StaticNodeKey &key, StaticVectorClock &sv) const;
  std::unordered_set<ThreadID> getDescendantThreadIDs(ThreadID tid) const;
  bool happensBefore(const StaticVectorClock &lhs,
                     const StaticVectorClock &rhs) const;
  bool svcLeq(const StaticVectorClock &lhs, const StaticVectorClock &rhs) const;
  void computeSVMax(const StaticVectorClock &sv1, const StaticVectorClock &sv2,
                    StaticVectorClock &out) const;

  // Queries
  void computeMHPPairs();

  bool isInstructionThreadAmbiguous(const llvm::Instruction *inst) const;
  bool isMustIntraThreadEdge(const SyncNode *from, const SyncNode *to) const;
  const llvm::PostDominatorTree &
  getPostDomTree(const llvm::Function *func) const;
  void enableIndirectForkConservatism();
  bool isMultiInstanceThread(ThreadID tid) const;

  // Thread bookkeeping reused from the TFG builder
  ThreadID m_next_thread_id = 1; // 0 reserved for main
  std::unordered_map<const llvm::Instruction *, ThreadID> m_inst_to_thread;
  std::unordered_map<ThreadID, const llvm::Instruction *> m_thread_fork_sites;
  std::unordered_map<ThreadID, ThreadID> m_thread_parents;
  std::unordered_map<ThreadID, std::vector<ThreadID>> m_thread_children;
  std::unordered_map<const llvm::Instruction *, ThreadID> m_fork_to_thread;
  std::unordered_map<const llvm::Instruction *, ThreadID> m_join_to_thread;
  std::unordered_set<ThreadID> m_detached_threads;
  std::unordered_map<const llvm::Value *, std::unordered_set<ThreadID>>
      m_pthread_value_to_threads;
  std::unordered_map<ThreadID, const llvm::Value *> m_thread_to_pthread_value;
  std::unordered_map<
      ThreadID, std::unordered_map<CallContextID,
                                   std::unordered_set<const llvm::Function *>>>
      m_visited_functions_by_thread;
  std::unordered_map<ThreadID, std::vector<const llvm::Function *>>
      m_active_call_stack_by_thread;
  std::unordered_map<const llvm::Value *, std::vector<SyncNode *>>
      m_condvar_signals;
  std::unordered_map<const llvm::Value *, std::vector<SyncNode *>>
      m_condvar_waits;
  struct BarrierParticipant {
    SyncNode *arrival = nullptr;
    std::vector<SyncNode *> continuations;
  };
  std::unordered_map<
      const llvm::Value *,
      std::unordered_map<size_t, std::vector<BarrierParticipant>>>
      m_barrier_waits;
  std::unordered_map<const llvm::Value *, std::unordered_map<ThreadID, size_t>>
      m_barrier_phase_by_thread;
  std::unordered_map<const llvm::Value *, std::unordered_map<ThreadID, size_t>>
      m_pending_split_barrier_phase_by_thread;
  std::unordered_map<const llvm::Value *, size_t> m_barrier_expected_counts;
  std::unordered_set<ThreadID> m_multi_instance_threads;

  // Indirect fork handling (conservative)
  bool m_has_unresolved_fork = false;
  std::unordered_set<const llvm::Function *> m_thread_entry_candidates;

  mutable std::unordered_map<const llvm::Function *,
                             std::unique_ptr<llvm::PostDominatorTree>>
      m_post_dom_cache;

  static constexpr ThreadID kUnknownThread =
      std::numeric_limits<ThreadID>::max();
};

} // namespace mhp

#endif // STATIC_VECTOR_CLOCK_MHP_H
