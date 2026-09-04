#pragma once

#include "CFL/Classical/Core/Graph.h"

#include <cstddef>
#include <vector>

namespace lotus::cfl::classical {

enum class GraphSimplificationFlavor {
  Alias,
  ValueFlow,
};

struct GraphSimplificationOptions {
  GraphSimplificationFlavor flavor = GraphSimplificationFlavor::Alias;
  bool eliminate_sccs = false;
  bool fold_graph = false;
  bool prune_interdyck = false;
};

struct GraphSimplificationStatistics {
  std::size_t original_nodes = 0;
  std::size_t original_edges = 0;
  std::size_t reduced_nodes = 0;
  std::size_t reduced_edges = 0;
  std::size_t sccs = 0;
  std::size_t scc_nodes_merged = 0;
  std::size_t folded_nodes = 0;
  std::size_t common_dereference_nodes_merged = 0;
  std::size_t interdyck_edges_pruned = 0;
};

struct GraphSimplificationResult {
  LabeledGraph graph;
  /// Original vertex -> reduced vertex.
  std::vector<std::size_t> representative;
  /// Reduced vertex -> original vertices.
  std::vector<std::vector<std::size_t>> members;
  GraphSimplificationStatistics statistics;
};

/// Port of POCR's SCC elimination and PEG/IVFG graph-folding passes.
GraphSimplificationResult
simplifyGraph(const LabeledGraph &graph,
              const GraphSimplificationOptions &options);

} // namespace lotus::cfl::classical
