#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lotus::cfl::classical {

struct ReachabilityStats {
  // Session snapshots after this solve.
  std::size_t graph_nodes = 0;
  std::size_t base_graph_edges = 0;
  std::size_t grammar_symbols = 0;
  std::size_t grammar_terminals = 0;
  std::size_t grammar_nonterminals = 0;
  std::size_t grammar_productions = 0;
  std::size_t grammar_nullable_symbols = 0;
  std::size_t grammar_transitive_symbols = 0;
  std::size_t input_edges = 0;
  std::size_t relation_edges = 0;
  std::size_t start_symbol_edges = 0;
  std::size_t relation_payload_bytes_estimate = 0;
  std::size_t transitive_closure_instances = 0;
  std::size_t transitive_relation_edges = 0;
  std::size_t transitive_payload_bytes_estimate = 0;

  // Work performed by this solve call only.
  std::uint64_t classical_iterations = 0;
  std::size_t processed_work_items = 0;
  std::size_t duplicate_edges = 0;
  std::size_t peak_worklist_size = 0;
  std::size_t added_edges = 0;
  std::uint64_t solve_time_microseconds = 0;
  std::size_t transitive_arc_insertions = 0;
  std::size_t transitive_propagated_pairs = 0;
  std::size_t transitive_duplicate_pairs = 0;

  // Aggregates report how many solve calls they combine.
  std::size_t solver_rounds = 1;
};

enum class SolverBackend {
  /// Classical worklist with hash-set relations.
  SparseSet,
  /// Same classical worklist with LLVM sparse-bitvector relations.
  SparseBitVector,
  /// Classical worklist plus dedicated incremental closure only for symbols
  /// having a literal production A -> A A.
  TransitiveClosure,
};

const char *solverBackendName(SolverBackend backend);

/// Retains the derived relation and supports adding terminal edges followed by
/// another run to a fixed point.
class SolverSession {
public:
  SolverSession(LabeledGraph &graph, const Grammar &grammar,
                SolverBackend backend = SolverBackend::SparseSet);
  ~SolverSession();
  SolverSession(SolverSession &&) noexcept;
  SolverSession &operator=(SolverSession &&) noexcept;
  SolverSession(const SolverSession &) = delete;
  SolverSession &operator=(const SolverSession &) = delete;

  std::size_t addNode(const std::string &name);
  bool addTerminalEdge(std::size_t source, std::size_t target,
                       const std::string &label);
  ReachabilityStats solve();
  bool contains(std::size_t source, std::size_t target,
                const std::string &label) const;
  const Relation &relation() const;

private:
  friend class AliasClient;

  /// Seed a previously derived fact while migrating to a monotonic grammar
  /// extension. This does not mutate the input graph.
  bool addKnownRelationEdge(std::size_t source, std::size_t target,
                            const std::string &label);

  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical
