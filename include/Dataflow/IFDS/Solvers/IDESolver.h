/*
 * IDE Solver
 *
 * This header provides the IDE (Interprocedural Distributive Environment)
 * solver for the IFDS framework with:
 * - Summary edge reuse for efficient interprocedural analysis
 * - Edge function composition memoization
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"
#include "Dataflow/IFDS/Core/IFDSIDESolverConfig.h"
#include "Dataflow/IFDS/Core/IFDSIDESolverStatistics.h"
#include "Dataflow/IFDS/Core/SolverGraphContext.h"

#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace ifds {

namespace detail {
// FNV-1a-based hash combiner — avoids the collision problems of XOR-shifting
// pointer hashes by small amounts (aligned pointers have zero low bits).
inline size_t fnv_mix(size_t h, size_t v) {
  h ^= v;
  h *= 1099511628211ULL;
  return h;
}

template <typename A, typename B, typename C> struct TripleHash {
  size_t operator()(const std::tuple<A, B, C> &t) const {
    size_t h = 14695981039346656037ULL;
    h = fnv_mix(h, std::hash<A>{}(std::get<0>(t)));
    h = fnv_mix(h, std::hash<B>{}(std::get<1>(t)));
    h = fnv_mix(h, std::hash<C>{}(std::get<2>(t)));
    return h;
  }
};
template <typename A, typename B, typename C> struct TripleEq {
  bool operator()(const std::tuple<A, B, C> &a,
                  const std::tuple<A, B, C> &b) const {
    return std::get<0>(a) == std::get<0>(b) &&
           std::get<1>(a) == std::get<1>(b) && std::get<2>(a) == std::get<2>(b);
  }
};
template <typename A, typename B, typename C, typename D> struct QuadHash {
  size_t operator()(const std::tuple<A, B, C, D> &t) const {
    size_t h = 14695981039346656037ULL;
    h = fnv_mix(h, std::hash<A>{}(std::get<0>(t)));
    h = fnv_mix(h, std::hash<B>{}(std::get<1>(t)));
    h = fnv_mix(h, std::hash<C>{}(std::get<2>(t)));
    h = fnv_mix(h, std::hash<D>{}(std::get<3>(t)));
    return h;
  }
};
template <typename A, typename B, typename C, typename D> struct QuadEq {
  bool operator()(const std::tuple<A, B, C, D> &a,
                  const std::tuple<A, B, C, D> &b) const {
    return std::get<0>(a) == std::get<0>(b) &&
           std::get<1>(a) == std::get<1>(b) &&
           std::get<2>(a) == std::get<2>(b) && std::get<3>(a) == std::get<3>(b);
  }
};
} // namespace detail

// ============================================================================
// IDE Solver
// ============================================================================

template <typename Problem> class IDESolver {
public:
  using Fact = typename Problem::FactType;
  using FactSet = typename Problem::FactSet;
  using Value = typename Problem::ValueType;
  using EdgeFunction = typename Problem::EdgeFunction;
  using EdgeFunctionPtr = std::shared_ptr<EdgeFunction>;
  using Node = typename ExplodedSupergraph<Fact>::Node;
  using PathEdgeType = PathEdge<Fact>;
  using PathEdgeHashType = PathEdgeHash<Fact>;

  IDESolver(Problem &problem);
  ~IDESolver();

  void solve(const llvm::Module &module);

  // Solver configuration (unbalanced returns, etc.)
  void set_solver_config(IFDSIDESolverConfig config) {
    m_config = std::move(config);
  }
  IFDSIDESolverConfig &get_solver_config() { return m_config; }
  const IFDSIDESolverConfig &get_solver_config() const { return m_config; }

  // Bounded solver: optional step limit (0 = unbounded). When the bound is
  // reached, the solver stops and returns a partial result.
  void set_max_steps(size_t max_steps) { m_max_steps = max_steps; }
  size_t get_max_steps() const { return m_max_steps; }
  size_t get_steps_performed() const { return m_steps_performed; }
  bool bound_reached() const { return m_bound_reached; }

  // Query interface
  Value get_value_at(const llvm::Instruction *inst, const Fact &fact) const;
  /// Returns value at the given instruction in LLVM SSA style: for non-void
  /// instructions, returns value at the successor where the def is valid.
  Value get_value_at_in_llvm_ssa(const llvm::Instruction *inst,
                                 const Fact &fact) const;
  const std::unordered_map<const llvm::Instruction *,
                           std::unordered_map<Fact, Value>> &
  get_all_values() const;
  void get_path_edges(std::vector<PathEdgeType> &out_edges) const;
  void get_summary_edges(std::vector<SummaryEdge<Fact>> &out_edges) const;
  const IFDSIDESolverStatistics &get_statistics() const { return m_statistics; }

protected:
  virtual void on_path_edge_added(const PathEdgeType &edge) { (void)edge; }
  virtual void on_summary_edge_added(const SummaryEdge<Fact> &edge) {
    (void)edge;
  }
  virtual void on_normal_transition(const Node &source, const Node &target) {
    (void)source;
    (void)target;
  }
  virtual void on_call_transition(const Node &source, const Node &target) {
    (void)source;
    (void)target;
  }
  virtual void on_return_transition(const Node &source, const Node &target) {
    (void)source;
    (void)target;
  }
  virtual void on_call_to_return_transition(const Node &source,
                                            const Node &target) {
    (void)source;
    (void)target;
  }
  virtual void on_summary_transition(const Node &source, const Node &target) {
    (void)source;
    (void)target;
  }

private:
  struct EndSummary {
    const llvm::Instruction *exit_inst;
    Fact exit_fact;
    EdgeFunctionPtr phi;
  };

  struct StartKey {
    const llvm::Instruction *start_node;
    Fact start_fact;

    bool operator==(const StartKey &other) const {
      return start_node == other.start_node && start_fact == other.start_fact;
    }
  };

  struct StartKeyHash {
    size_t operator()(const StartKey &key) const {
      size_t h = 14695981039346656037ULL;
      h = detail::fnv_mix(
          h, std::hash<const llvm::Instruction *>{}(key.start_node));
      h = detail::fnv_mix(h, std::hash<Fact>{}(key.start_fact));
      return h;
    }
  };

  struct IncomingEdge {
    const llvm::CallBase *call;
    Fact call_fact;
    const llvm::Instruction *start_node;
    Fact start_fact;

    bool operator==(const IncomingEdge &other) const {
      return call == other.call && call_fact == other.call_fact &&
             start_node == other.start_node && start_fact == other.start_fact;
    }
  };

  // Composition cache key
  struct ComposePair {
    EdgeFunctionPtr f1;
    EdgeFunctionPtr f2;

    bool operator==(const ComposePair &other) const {
      return f1 == other.f1 && f2 == other.f2;
    }
  };

  struct ComposePairHash {
    size_t operator()(const ComposePair &cp) const {
      // FNV-1a-style mixing to avoid XOR-shift collisions on aligned pointers.
      size_t h = 14695981039346656037ULL;
      h ^= std::hash<EdgeFunctionPtr>{}(cp.f1);
      h *= 1099511628211ULL;
      h ^= std::hash<EdgeFunctionPtr>{}(cp.f2);
      h *= 1099511628211ULL;
      return h;
    }
  };

  // Helper: memoized composition
  EdgeFunctionPtr compose_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2);
  // Helper: memoized join for jump-function updates
  EdgeFunctionPtr join_cached(EdgeFunctionPtr f1, EdgeFunctionPtr f2);
  bool join_contains(EdgeFunctionPtr aggregate, EdgeFunctionPtr member) const;
  void record_join_members(EdgeFunctionPtr aggregate, EdgeFunctionPtr f1,
                           EdgeFunctionPtr f2);

  // Helper: create shared pointer to edge function
  EdgeFunctionPtr make_edge_function(const EdgeFunction &ef);

  Problem &m_problem;
  IFDSIDESolverConfig m_config;
  IFDSIDESolverStatistics m_statistics;
  SolverGraphContext<Fact, Problem> m_graph_context;
  std::unique_ptr<lotus::AliasAnalysisWrapper> m_owned_alias_analysis;
  bool m_injected_alias_analysis = false;

  // Bounded solver state (0 = unbounded)
  size_t m_max_steps = 0;
  size_t m_steps_performed = 0;
  bool m_bound_reached = false;

  // Results: instruction -> fact -> value
  std::unordered_map<const llvm::Instruction *, std::unordered_map<Fact, Value>>
      m_values;

  // Jump functions: path edge -> joined edge function
  std::unordered_map<PathEdgeType, EdgeFunctionPtr, PathEdgeHashType>
      m_jump_functions;

  // Incoming call edges for each callee start fact
  std::unordered_map<StartKey, std::vector<IncomingEdge>, StartKeyHash>
      m_incoming;

  // End summaries per callee start fact.
  std::unordered_map<StartKey, std::vector<EndSummary>, StartKeyHash>
      m_end_summaries;

  // Composition memoization table
  std::unordered_map<ComposePair, EdgeFunctionPtr, ComposePairHash>
      m_compose_cache;
  std::unordered_map<ComposePair, EdgeFunctionPtr, ComposePairHash>
      m_join_cache;
  std::unordered_map<const EdgeFunction *,
                     std::unordered_set<const EdgeFunction *>>
      m_join_members;

  // Edge function caches (avoid recomputing same edge function)
  using NormalEdgeKey = std::tuple<const llvm::Instruction *,
                                   const llvm::Instruction *, Fact, Fact>;
  using CallToReturnEdgeKey =
      std::tuple<const llvm::CallBase *, const llvm::Instruction *, Fact, Fact>;
  std::unordered_map<NormalEdgeKey, EdgeFunctionPtr,
                     detail::QuadHash<const llvm::Instruction *,
                                      const llvm::Instruction *, Fact, Fact>,
                     detail::QuadEq<const llvm::Instruction *,
                                    const llvm::Instruction *, Fact, Fact>>
      m_normal_edge_cache;
  std::unordered_map<CallToReturnEdgeKey, EdgeFunctionPtr,
                     detail::QuadHash<const llvm::CallBase *,
                                      const llvm::Instruction *, Fact, Fact>,
                     detail::QuadEq<const llvm::CallBase *,
                                    const llvm::Instruction *, Fact, Fact>>
      m_call_to_return_edge_cache;

  // Worklist of path edges with edge functions
  std::vector<std::pair<PathEdgeType, EdgeFunctionPtr>> m_worklist;
  std::unordered_set<PathEdgeType, PathEdgeHashType> m_path_edges;
  std::set<SummaryEdge<Fact>> m_summary_edges;
};

} // namespace ifds

#include "Dataflow/IFDS/Solvers/IDESolver.tpp"
