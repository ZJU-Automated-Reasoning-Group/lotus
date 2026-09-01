#pragma once

#include "CFL/Classical/Grammar.h"
#include "CFL/Classical/Graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lotus::cfl::classical {

struct ReachabilityStats {
  std::size_t graph_nodes = 0;
  std::size_t base_graph_edges = 0;
  std::size_t grammar_symbols = 0;
  std::size_t grammar_terminals = 0;
  std::size_t grammar_nonterminals = 0;
  std::size_t grammar_productions = 0;
  std::size_t grammar_nullable_symbols = 0;
  std::size_t grammar_transitive_symbols = 0;
  std::uint64_t classical_iterations = 0;
  std::size_t processed_work_items = 0;
  std::size_t duplicate_edges = 0;
  std::size_t peak_worklist_size = 0;
  std::size_t added_edges = 0;
  std::size_t input_edges = 0;
  std::size_t relation_edges = 0;
  std::size_t start_symbol_edges = 0;
  std::uint64_t solve_time_microseconds = 0;
  std::size_t relation_memory_bytes = 0;
  std::size_t solver_rounds = 1;
  std::size_t hybrid_forest_roots = 0;
  std::size_t hybrid_forest_nodes = 0;
  std::size_t hybrid_forest_edges = 0;
  std::size_t hybrid_arc_insertions = 0;
  std::size_t hybrid_meld_operations = 0;
  std::size_t hybrid_duplicate_melds = 0;
  std::size_t hybrid_forest_memory_bytes = 0;
};

enum class SolverBackend {
  Baseline,
  POCR,
  Hybrid,
};

const char *solverBackendName(SolverBackend backend);

/// Retains the derived relation and supports adding terminal edges followed by
/// another run to a fixed point.
class SolverSession {
public:
  SolverSession(LabeledGraph &graph, const Grammar &grammar,
                SolverBackend backend = SolverBackend::Baseline);
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
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical
