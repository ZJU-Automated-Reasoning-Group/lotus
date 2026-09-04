#pragma once

#include "CFL/InterleavedDyck/Core/BidirectedDyck.h"
#include "CFL/InterleavedDyck/Core/UnaryGraph.h"

#include <cstddef>
#include <unordered_map>

namespace lotus::cfl::interleaved_dyck::unary {

using interleaved_dyck::BidirectedInputPolicy;
using interleaved_dyck::Graph;
using interleaved_dyck::Vertex;

struct FixedCounterStats {
  std::size_t input_vertices = 0;
  std::size_t input_arcs = 0;
  std::size_t quotient_vertices = 0;
  std::size_t quotient_arcs = 0;
  std::size_t counter_bound = 0;
  std::size_t control_states = 0;
  std::size_t translated_arcs = 0;
  std::size_t epsilon_edges = 0;
  std::size_t closing_edges = 0;
  std::size_t added_reverse_arcs = 0;
  bool input_was_bidirected = true;
  bool overapproximates_original = false;
  bool sparsified = false;
  interleaved_dyck::BidirectedDyckStats quotient_dyck;
  interleaved_dyck::BidirectedDyckStats dyck;
};

class FixedCounterResult {
public:
  const std::unordered_map<Vertex, std::size_t> &components() const {
    return components_;
  }
  const FixedCounterStats &stats() const { return stats_; }

  std::size_t component(Vertex vertex) const;
  bool connected(Vertex first, Vertex second) const;

private:
  friend class FixedCounterSolver;

  std::unordered_map<Vertex, std::size_t> components_;
  FixedCounterStats stats_;
};

struct FixedCounterOptions {
  /// Apply the fixed-alphabet sparsification assumed by the POPL 2022 paper.
  bool sparsify = true;

  BidirectedInputPolicy input_policy = BidirectedInputPolicy::RequireBidirected;
};

/// Exact bounded-path algorithm of Kjelstrom and Pavlogiannis (POPL 2022).
///
/// Counter 2 is stored in finite control up to 18*n^2+6*n; counter 1 remains
/// the height of a bidirected one-counter graph.
class FixedCounterSolver {
public:
  FixedCounterResult solve(const Graph &graph,
                           const FixedCounterOptions &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck::unary
