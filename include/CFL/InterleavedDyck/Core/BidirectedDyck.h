#pragma once

#include <cstddef>
#include <vector>

namespace lotus::cfl::interleaved_dyck {

struct StatePair {
  std::size_t source = 0;
  std::size_t target = 0;
};

struct LabeledStateEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  std::size_t label = 0;
};

struct BidirectedDyckStats {
  std::size_t states = 0;
  std::size_t epsilon_edges = 0;
  std::size_t closing_edges = 0;
  std::size_t worklist_pops = 0;
  std::size_t component_unions = 0;
  std::size_t components = 0;
};

struct BidirectedDyckResult {
  std::vector<std::size_t> component;
  BidirectedDyckStats stats;
};

/// Compute zero-height Dyck components of a bidirected one-counter graph.
///
/// Only closing edges are represented explicitly; bidirectedness supplies the
/// corresponding opening reverses. `label_count` supports the fixed-alphabet
/// multi-type closure used during quotient sparsification.
class BidirectedDyckComponentSolver {
public:
  BidirectedDyckResult
  solve(std::size_t state_count, std::size_t label_count,
        const std::vector<StatePair> &epsilon_edges,
        const std::vector<LabeledStateEdge> &closing_edges) const;
};

} // namespace lotus::cfl::interleaved_dyck
