/*
 * IFDS/IDE Framework
 *
 * This header provides a comprehensive IFDS/IDE framework
 */

#pragma once

#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
#include "Utils/Parallel/ThreadSafe.h"

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

// Forward declaration
namespace lotus {
class AliasAnalysisWrapper;
} // namespace lotus

namespace ifds {

// Import thread-safe data structures from Support/ADT
using lotus::ShardedMap;
using lotus::SimpleOptional;
using lotus::ThreadSafeMap;
using lotus::ThreadSafeSet;
using lotus::ThreadSafeVector;

// ============================================================================
// Forward Declarations
// ============================================================================

template <typename Fact> class IFDSProblem;
template <typename Fact, typename Value> class IDEProblem;
template <typename Fact> class ExplodedSupergraph;

// ============================================================================
// IFDS Core Data Structures
// ============================================================================

// Helper for fact comparison so clients can specialize (e.g. for types without
// visible operator< at template instantiation).
template <typename Fact> bool fact_less(const Fact &a, const Fact &b) {
  return a < b;
}

template <typename Fact> struct PathEdge {
  const llvm::Instruction *start_node;
  Fact start_fact;
  const llvm::Instruction *target_node;
  Fact target_fact;

  PathEdge(const llvm::Instruction *s_node, const Fact &s_fact,
           const llvm::Instruction *t_node, const Fact &t_fact)
      : start_node(s_node), start_fact(s_fact), target_node(t_node),
        target_fact(t_fact) {}

  bool operator==(const PathEdge &other) const {
    return start_node == other.start_node && target_node == other.target_node &&
           start_fact == other.start_fact && target_fact == other.target_fact;
  }
  bool operator<(const PathEdge &other) const {
    if (start_node != other.start_node)
      return start_node < other.start_node;
    if (target_node != other.target_node)
      return target_node < other.target_node;
    if (start_fact != other.start_fact)
      return fact_less(start_fact, other.start_fact);
    return fact_less(target_fact, other.target_fact);
  }
};

template <typename Fact> struct PathEdgeHash {
  size_t operator()(const PathEdge<Fact> &edge) const {
    // Use a proper hash combiner (FNV-style) to avoid the collision
    // problems of XOR-shifting pointer hashes by small amounts.
    // 64-bit pointers are typically 8-byte aligned so their low 3 bits
    // are always zero; shifting by 1/2/3 and XOR-ing produces many
    // collisions.  Multiplying by a large prime disperses the bits.
    size_t h = 14695981039346656037ULL;
    auto mix = [&h](size_t v) {
      h ^= v;
      h *= 1099511628211ULL;
    };
    mix(std::hash<const llvm::Instruction *>{}(edge.start_node));
    mix(std::hash<const llvm::Instruction *>{}(edge.target_node));
    mix(std::hash<Fact>{}(edge.start_fact));
    mix(std::hash<Fact>{}(edge.target_fact));
    return h;
  }
};

template <typename Fact> struct SummaryEdge {
  const llvm::CallBase *call_site;
  const llvm::Instruction *return_site;
  Fact call_fact;
  Fact return_fact;

  SummaryEdge(const llvm::CallBase *call, const llvm::Instruction *ret_site,
              const Fact &c_fact, const Fact &r_fact)
      : call_site(call), return_site(ret_site), call_fact(c_fact),
        return_fact(r_fact) {}

  bool operator==(const SummaryEdge &other) const {
    return call_site == other.call_site && return_site == other.return_site &&
           call_fact == other.call_fact && return_fact == other.return_fact;
  }
  bool operator<(const SummaryEdge &other) const {
    if (call_site != other.call_site)
      return call_site < other.call_site;
    if (return_site != other.return_site)
      return return_site < other.return_site;
    if (call_fact != other.call_fact)
      return fact_less(call_fact, other.call_fact);
    return fact_less(return_fact, other.return_fact);
  }
};

template <typename Fact> struct SummaryEdgeHash {
  size_t operator()(const SummaryEdge<Fact> &edge) const {
    // Use FNV-1a-style mixing to avoid the collision problems of
    // XOR-shifting aligned pointer/fact hashes by small amounts.
    size_t h = 14695981039346656037ULL;
    auto mix = [&h](size_t v) {
      h ^= v;
      h *= 1099511628211ULL;
    };
    mix(std::hash<const llvm::CallBase *>{}(edge.call_site));
    mix(std::hash<const llvm::Instruction *>{}(edge.return_site));
    mix(std::hash<Fact>{}(edge.call_fact));
    mix(std::hash<Fact>{}(edge.return_fact));
    return h;
  }
};

// ============================================================================
// Initial Seeds Representation
// ============================================================================

template <typename Fact> struct InitialSeeds {
  using FactSet = std::set<Fact>;
  using SeedMap = std::unordered_map<const llvm::Instruction *, FactSet>;

  void add_seed(const llvm::Instruction *inst, const Fact &fact) {
    seeds[inst].insert(fact);
  }

  void add_seed(const llvm::Instruction *inst, const FactSet &facts) {
    auto &set = seeds[inst];
    set.insert(facts.begin(), facts.end());
  }

  const SeedMap &get_seeds() const { return seeds; }
  bool empty() const { return seeds.empty(); }

  SeedMap seeds;
};

template <typename Fact, typename Value> struct IDEInitialSeeds {
  using ValueMap = std::unordered_map<Fact, Value>;
  using SeedMap = std::unordered_map<const llvm::Instruction *, ValueMap>;

  void add_seed(const llvm::Instruction *inst, const Fact &fact,
                const Value &value) {
    seeds[inst][fact] = value;
  }

  void add_seed(const llvm::Instruction *inst, const ValueMap &facts) {
    auto &map = seeds[inst];
    map.insert(facts.begin(), facts.end());
  }

  void add_seed_instruction(const llvm::Instruction *inst) {
    (void)seeds[inst];
  }

  const SeedMap &get_seeds() const { return seeds; }
  bool empty() const { return seeds.empty(); }

  SeedMap seeds;
};

// ============================================================================
// IFDS Problem Interface
// ============================================================================

template <typename Fact> class IFDSProblem {
public:
  using FactType = Fact;
  using FactSet = std::set<Fact>;
  using InitialSeeds = ifds::InitialSeeds<Fact>;

  virtual ~IFDSProblem() = default;

  // Zero fact (lambda in IFDS terminology)
  virtual Fact zero_fact() const = 0;

  // Flow functions for different statement types
  virtual FactSet normal_flow(const llvm::Instruction *stmt,
                              const llvm::Instruction *succ,
                              const Fact &fact) = 0;
  virtual FactSet call_flow(const llvm::CallBase *call,
                            const llvm::Function *callee, const Fact &fact) = 0;
  virtual FactSet return_flow(const llvm::CallBase *call,
                              const llvm::Instruction *exit_inst,
                              const llvm::Instruction *return_site,
                              const llvm::Function *callee,
                              const Fact &exit_fact, const Fact &call_fact) = 0;
  virtual FactSet call_to_return_flow(
      const llvm::CallBase *call, const llvm::Instruction *return_site,
      llvm::ArrayRef<const llvm::Function *> callees, const Fact &fact) = 0;
  virtual FactSet summary_flow(const llvm::CallBase * /*call*/,
                               const llvm::Function * /*callee*/,
                               const Fact & /*fact*/) {
    return {};
  }

  // Initial facts at program entry
  virtual FactSet initial_facts(const llvm::Function *main) = 0;

  // Optional initial seeds override (multiple entry points)
  virtual InitialSeeds initial_seeds(const llvm::Module &module);

  // Zero-fact handling (auto-add and identity preservation)
  virtual bool auto_add_zero() const { return true; }
  virtual bool is_zero_fact(const Fact &fact) const {
    return fact == zero_fact();
  }

  // Alias analysis integration
  virtual void set_alias_analysis(lotus::AliasAnalysisWrapper *aa);
  bool has_alias_analysis_configured() const;

  // Helper methods for common operations
  virtual bool is_source(const llvm::Instruction *inst) const;
  virtual bool is_sink(const llvm::Instruction *inst) const;

protected:
  lotus::AliasAnalysisWrapper *m_alias_analysis = nullptr;

  // Alias analysis helper using AliasAnalysisWrapper
  bool may_alias(const llvm::Value *v1, const llvm::Value *v2) const;
};

// ============================================================================
// No-Alias IFDS/IDE Problem Bases (Phasar-style split)
// ============================================================================

template <typename Fact>
class DefaultNoAliasIFDSProblem : public IFDSProblem<Fact> {};

// ============================================================================
// Alias-Aware IFDS Problem Base
// ============================================================================

template <typename Fact>
class DefaultAliasAwareIFDSProblem : public IFDSProblem<Fact> {
public:
  using typename IFDSProblem<Fact>::FactSet;

protected:
  bool has_alias_analysis() const {
    return this->m_alias_analysis != nullptr &&
           this->m_alias_analysis->isInitialized();
  }

  // IFDS/IDE default alias-aware modeling is intentionally pairwise only.
  // Clients must not assume the backend can enumerate complete alias sets.
  bool may_alias_or_equal(const llvm::Value *v1, const llvm::Value *v2) const {
    if (v1 == v2) {
      return true;
    }
    return this->may_alias(v1, v2);
  }
};

// ============================================================================
// IDE Problem Interface
// ============================================================================

template <typename Fact, typename Value>
class IDEProblem : public IFDSProblem<Fact> {
public:
  using ValueType = Value;
  using EdgeFunction = std::function<Value(const Value &)>;
  using FactSet = typename IFDSProblem<Fact>::FactSet;
  using IDEInitialSeeds = ifds::IDEInitialSeeds<Fact, Value>;

  // Edge functions for IDE
  virtual EdgeFunction normal_edge_function(const llvm::Instruction *stmt,
                                            const llvm::Instruction *succ,
                                            const Fact &src_fact,
                                            const Fact &tgt_fact) = 0;
  virtual EdgeFunction call_edge_function(const llvm::CallBase *call,
                                          const llvm::Function *callee,
                                          const Fact &src_fact,
                                          const Fact &tgt_fact) = 0;
  virtual EdgeFunction
  return_edge_function(const llvm::CallBase *call, const llvm::Function *callee,
                       const llvm::Instruction *exit_inst,
                       const llvm::Instruction *return_site,
                       const Fact &exit_fact, const Fact &ret_fact) = 0;
  virtual EdgeFunction
  call_to_return_edge_function(const llvm::CallBase *call,
                               const llvm::Instruction *return_site,
                               llvm::ArrayRef<const llvm::Function *> callees,
                               const Fact &src_fact, const Fact &tgt_fact) = 0;
  // Optional summary flow/edge functions (for special-cased callees)
  virtual FactSet summary_flow(const llvm::CallBase * /*call*/,
                               const llvm::Function * /*callee*/,
                               const Fact & /*fact*/) {
    return {};
  }
  virtual EdgeFunction
  summary_edge_function(const llvm::CallBase * /*call*/,
                        const llvm::Function * /*callee*/,
                        const llvm::Instruction * /*return_site*/,
                        const Fact & /*src_fact*/, const Fact & /*tgt_fact*/) {
    return identity();
  }

  // Value domain operations
  virtual Value top_value() const = 0;
  virtual Value bottom_value() const = 0;
  virtual Value join(const Value &v1, const Value &v2) const = 0;

  // IDE initial seeds (Phasar-style: statement -> fact -> lattice value).
  virtual IDEInitialSeeds initial_ide_seeds(const llvm::Module &module) = 0;

  // Edge function composition
  virtual EdgeFunction compose(const EdgeFunction &f1,
                               const EdgeFunction &f2) const;
  // Edge function join (meet-over-all-paths merge on jump functions)
  virtual EdgeFunction join_edge_functions(const EdgeFunction &f1,
                                           const EdgeFunction &f2) const;
  // Heuristic equality check used to suppress redundant jump-function updates.
  virtual bool edge_function_equivalent(const EdgeFunction &f1,
                                        const EdgeFunction &f2) const;

  // Identity edge function
  EdgeFunction identity() const;

protected:
  IDEInitialSeeds lift_ifds_initial_seeds(const llvm::Module &module,
                                          const Value &seed_value);
};

template <typename Fact, typename Value>
class DefaultNoAliasIDEProblem : public IDEProblem<Fact, Value> {};

template <typename Fact, typename Value>
class DefaultAliasAwareIDEProblem : public IDEProblem<Fact, Value> {
protected:
  bool has_alias_analysis() const {
    return this->m_alias_analysis != nullptr &&
           this->m_alias_analysis->isInitialized();
  }

  // IFDS/IDE default alias-aware modeling is intentionally pairwise only.
  // Clients must not assume the backend can enumerate complete alias sets.
  bool may_alias_or_equal(const llvm::Value *v1, const llvm::Value *v2) const {
    if (v1 == v2) {
      return true;
    }
    return this->may_alias(v1, v2);
  }
};

// ============================================================================
// Exploded Supergraph Representation
// ============================================================================

template <typename Fact> class ExplodedSupergraph {
public:
  struct Node {
    const llvm::Instruction *instruction;
    Fact fact;

    Node() : instruction(nullptr), fact() {}
    Node(const llvm::Instruction *inst, const Fact &f)
        : instruction(inst), fact(f) {}

    bool operator==(const Node &other) const {
      return instruction == other.instruction && fact == other.fact;
    }
    bool operator<(const Node &other) const {
      if (instruction != other.instruction)
        return instruction < other.instruction;
      return fact < other.fact;
    }
  };

  struct NodeHash {
    size_t operator()(const Node &node) const {
      // Use FNV-1a-style mixing to avoid the collision problems of
      // XOR-shifting aligned pointer hashes by 1 bit.
      size_t h = 14695981039346656037ULL;
      h ^= std::hash<const llvm::Instruction *>{}(node.instruction);
      h *= 1099511628211ULL;
      h ^= std::hash<Fact>{}(node.fact);
      h *= 1099511628211ULL;
      return h;
    }
  };

  struct Edge {
    Node source;
    Node target;
    enum Type { NORMAL, CALL, RETURN, CALL_TO_RETURN } type;

    Edge(const Node &src, const Node &tgt, Type t)
        : source(src), target(tgt), type(t) {}
  };

  using NodeId = Node;
  using EdgeId = Edge;
  using Graph = ExplodedSupergraph<Fact>;

  // GraphInterface implementation for fixpoint iterator
  static NodeId entry(const Graph &graph);
  static NodeId source(const Graph &graph, const EdgeId &edge);
  static NodeId target(const Graph &graph, const EdgeId &edge);
  static std::vector<EdgeId> predecessors(const Graph &graph,
                                          const NodeId &node);
  static std::vector<EdgeId> successors(const Graph &graph, const NodeId &node);

  void add_edge(const Edge &edge);
  void set_entry(const NodeId &entry);
  const std::vector<Edge> &get_edges() const;

private:
  std::unique_ptr<NodeId> m_entry;
  std::vector<Edge> m_edges;
  std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_successors;
  std::unordered_map<NodeId, std::vector<EdgeId>, NodeHash> m_predecessors;
};

// ============================================================================
// IFDS/IDE Solvers
// ============================================================================
// Solver declarations: include/Dataflow/IFDS/Solvers/IFDSSolver.h and
// include/Dataflow/IFDS/Solvers/IDESolver.h

} // namespace ifds

// Provide std::hash specializations for IFDS types used in unordered containers
namespace std {
template <typename Fact> struct hash<ifds::PathEdge<Fact>> {
  size_t operator()(const ifds::PathEdge<Fact> &edge) const noexcept {
    return ifds::PathEdgeHash<Fact>{}(edge);
  }
};

template <typename Fact> struct hash<ifds::SummaryEdge<Fact>> {
  size_t operator()(const ifds::SummaryEdge<Fact> &edge) const noexcept {
    return ifds::SummaryEdgeHash<Fact>{}(edge);
  }
};
} // namespace std

// ============================================================================
// Template Implementation (moved to .cpp for explicit instantiation)
// ============================================================================

// Provide inline template implementations for commonly used helpers so that
// templated clients (e.g., taint analysis) can link without relying on a
// separate translation unit.

namespace ifds {

template <typename Fact>
inline void
IFDSProblem<Fact>::set_alias_analysis(lotus::AliasAnalysisWrapper *aa) {
  m_alias_analysis = aa;
}

template <typename Fact>
inline bool IFDSProblem<Fact>::has_alias_analysis_configured() const {
  return m_alias_analysis != nullptr;
}

template <typename Fact>
inline bool IFDSProblem<Fact>::is_source(const llvm::Instruction *) const {
  return false;
}

template <typename Fact>
inline bool IFDSProblem<Fact>::is_sink(const llvm::Instruction *) const {
  return false;
}

template <typename Fact>
inline bool IFDSProblem<Fact>::may_alias(const llvm::Value *v1,
                                         const llvm::Value *v2) const {
  if (!m_alias_analysis || !v1 || !v2)
    return false;
  return m_alias_analysis->mayAlias(v1, v2);
}

template <typename Fact>
inline typename IFDSProblem<Fact>::InitialSeeds
IFDSProblem<Fact>::initial_seeds(const llvm::Module &module) {
  InitialSeeds seeds;
  const llvm::Function *main_func = module.getFunction("main");
  if (!main_func || main_func->empty()) {
    return seeds;
  }

  const llvm::Instruction *entry = &main_func->getEntryBlock().front();
  seeds.add_seed(entry, initial_facts(main_func));
  return seeds;
}

// ============================================================================
// IDEProblem Inline Implementations
// ============================================================================

template <typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::EdgeFunction
IDEProblem<Fact, Value>::compose(const EdgeFunction &f1,
                                 const EdgeFunction &f2) const {
  return [f1, f2](const Value &v) { return f1(f2(v)); };
}

template <typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::EdgeFunction
IDEProblem<Fact, Value>::join_edge_functions(const EdgeFunction &f1,
                                             const EdgeFunction &f2) const {
  return [this, f1, f2](const Value &v) { return join(f1(v), f2(v)); };
}

template <typename Fact, typename Value>
inline bool IDEProblem<Fact, Value>::edge_function_equivalent(
    const EdgeFunction &f1, const EdgeFunction &f2) const {
  const Value top = top_value();
  const Value bottom = bottom_value();
  std::vector<Value> probes;
  probes.push_back(top);
  if (!(bottom == top)) {
    probes.push_back(bottom);
  }

  const Value join_tb = join(top, bottom);
  if (!(join_tb == top) && !(join_tb == bottom)) {
    probes.push_back(join_tb);
  }
  const Value join_bt = join(bottom, top);
  if (!(join_bt == top) && !(join_bt == bottom) && !(join_bt == join_tb)) {
    probes.push_back(join_bt);
  }

  constexpr size_t MAX_PROBES = 16;
  constexpr size_t MAX_ROUNDS = 3;
  size_t idx = 0;
  size_t rounds = 0;

  while (idx < probes.size()) {
    const Value &probe = probes[idx++];
    Value out1 = f1(probe);
    Value out2 = f2(probe);
    if (!(out1 == out2)) {
      return false;
    }

    if (rounds < MAX_ROUNDS && probes.size() < MAX_PROBES) {
      bool seen1 = false;
      for (const Value &v : probes) {
        if (v == out1) {
          seen1 = true;
          break;
        }
      }
      if (!seen1) {
        probes.push_back(out1);
      }

      bool seen2 = false;
      for (const Value &v : probes) {
        if (v == out2) {
          seen2 = true;
          break;
        }
      }
      if (!seen2 && probes.size() < MAX_PROBES) {
        probes.push_back(out2);
      }
    }

    if (idx == probes.size()) {
      ++rounds;
    }
  }

  return true;
}

template <typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::IDEInitialSeeds
IDEProblem<Fact, Value>::lift_ifds_initial_seeds(const llvm::Module &module,
                                                 const Value &seed_value) {
  IDEInitialSeeds ide_seeds;
  auto ifds_seeds = this->initial_seeds(module);
  for (const auto &entry : ifds_seeds.get_seeds()) {
    const llvm::Instruction *inst = entry.first;
    ide_seeds.add_seed_instruction(inst);
    for (const Fact &fact : entry.second) {
      ide_seeds.add_seed(inst, fact, seed_value);
    }
  }
  return ide_seeds;
}

template <typename Fact, typename Value>
inline typename IDEProblem<Fact, Value>::EdgeFunction
IDEProblem<Fact, Value>::identity() const {
  return [](const Value &v) { return v; };
}

} // namespace ifds
