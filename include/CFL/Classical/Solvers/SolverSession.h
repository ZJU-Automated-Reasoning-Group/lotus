#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
  std::size_t count_symbol_edges = 0;
  std::size_t relation_payload_bytes_estimate = 0;
  std::size_t transitive_closure_instances = 0;
  std::size_t transitive_relation_edges = 0;
  std::size_t transitive_payload_bytes_estimate = 0;
  std::size_t pocr_tree_roots = 0;
  std::size_t pocr_tree_nodes = 0;
  std::size_t pocr_tree_edges = 0;
  std::size_t fully_ordered_critical_edges = 0;
  std::size_t candidate_relation_edges = 0;
  std::size_t specialized_reachability_pairs = 0;
  std::size_t specialized_matched_pairs = 0;
  std::size_t specialized_critical_edges = 0;

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
  std::size_t pocr_traversal_steps = 0;
  std::size_t pocr_tree_join_visits = 0;
  std::size_t fully_ordered_reachability_checks = 0;
  std::size_t fully_ordered_tree_join_visits = 0;
  std::size_t fully_ordered_critical_edge_insertions = 0;
  std::size_t fully_ordered_critical_edge_removals = 0;
  std::size_t fully_ordered_cycle_simplifications = 0;
  std::size_t graspan_epochs = 0;
  std::size_t endpoint_quotient_cells = 0;
  std::size_t endpoint_quotient_facts = 0;
  std::size_t endpoint_quotient_seed_facts = 0;
  std::size_t endpoint_quotient_inferred_facts = 0;
  std::size_t endpoint_quotient_binary_joins = 0;
  std::size_t endpoint_quotient_bridge_pairs = 0;
  std::size_t endpoint_quotient_source_classes = 0;
  std::size_t endpoint_quotient_target_classes = 0;
  std::size_t endpoint_quotient_nullable_symbols = 0;
  std::uint64_t endpoint_quotient_preprocess_us = 0;
  std::uint64_t endpoint_quotient_saturation_us = 0;
  std::uint64_t endpoint_quotient_count_us = 0;

  // Aggregates report how many solve calls they combine.
  std::size_t solver_rounds = 1;
};

enum class SolverBackend {
  /// Classical worklist with hash-set relations.
  SparseSet,
  /// Same classical worklist with LLVM sparse-bitvector relations.
  SparseBitVector,
  /// Graspan-style epoch/delta evaluation with sparse-bitvector relations.
  Graspan,
  /// Sqid adaptive and differential relation chaining.
  Sqid,
  /// PEARL transitivity-aware multi-derivation.
  Pearl,
  /// Classical worklist plus dedicated incremental closure only for symbols
  /// having a literal production A -> A A.
  TransitiveClosure,
  /// POCR paired predecessor/successor reachability-tree propagation.
  Pocr,
  /// POCR with transitive facts prioritized ahead of ordinary work items.
  HierarchicalPocr,
  /// Fully ordered CFL reachability with an edge-critical graph.
  FullyOrdered,
  /// Grammar-indexed endpoint-quotient (GEQ) compressed exact solving.
  EndpointQuotient,
};

struct SolverOptions {
  SolverBackend backend = SolverBackend::SparseSet;
  /// Honor POCR Insert/Follow metadata by indexing only terminals, nullable
  /// seeds, and Insert symbols as future join candidates.
  bool unidirectional = false;
  /// Apply POCR's optional ECG SCC simplification in the FOCR backend.
  bool simplify_focr_cycles = false;
  /// Explicit X/Xbar pairs for PEARL's PackRR and paired propagation graphs.
  std::vector<std::pair<std::string, std::string>> pearl_inverse_relations;
};

const char *solverBackendName(SolverBackend backend);
SolverBackend parseSolverBackend(std::string_view name);

/// Retains the derived relation and supports adding terminal edges followed by
/// another run to a fixed point.
class SolverSession {
public:
  SolverSession(LabeledGraph &graph, const Grammar &grammar,
                SolverBackend backend = SolverBackend::SparseSet);
  SolverSession(LabeledGraph &graph, const Grammar &grammar,
                const SolverOptions &options);
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
