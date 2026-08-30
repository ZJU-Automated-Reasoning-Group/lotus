#pragma once

#include "CFL/InterleavedDyckCore/UnaryGraph.h"

#include <cstddef>
#include <unordered_map>

namespace lotus::cfl::unary_interleaved_dyck {

using interleaved_dyck::BidirectedInputPolicy;
using interleaved_dyck::Graph;
using interleaved_dyck::Vertex;

/// Construction statistics for adaptive unary interleaved-Dyck reachability.
struct AdaptiveStats {
  std::size_t input_vertices = 0;
  std::size_t input_arcs = 0;
  std::size_t quotient_vertices = 0;
  std::size_t quotient_arcs = 0;
  std::size_t threshold = 0;
  std::size_t vertical_control_states = 0;
  std::size_t vertical_arcs = 0;
  std::size_t horizontal_control_states = 0;
  std::size_t horizontal_arcs = 0;
  std::size_t added_reverse_arcs = 0;
  bool input_was_bidirected = true;
  bool overapproximates_original = false;
  bool sparsified = false;
  interleaved_dyck::BidirectedDyckStats quotient_dyck;
  interleaved_dyck::BidirectedDyckStats vertical_dyck;
  interleaved_dyck::BidirectedDyckStats horizontal_dyck;
};

/// A component partition of the input vertices.
class AdaptiveResult {
public:
  const std::unordered_map<Vertex, std::size_t> &components() const {
    return components_;
  }
  const AdaptiveStats &stats() const { return stats_; }

  std::size_t component(Vertex vertex) const;
  bool connected(Vertex first, Vertex second) const;

private:
  friend class AdaptiveSolver;

  std::unordered_map<Vertex, std::size_t> components_;
  AdaptiveStats stats_;
};

struct AdaptiveOptions {
  /// Apply the reachability-preserving fixed-alphabet quotient before the two
  /// arm computations. This is the paper's end-to-end algorithm.
  bool sparsify = true;

  BidirectedInputPolicy input_policy = BidirectedInputPolicy::RequireBidirected;
};

/// Exact all-pairs reachability for bidirected unary D1-interleaved-D1 graphs.
///
/// Typed labels are projected as follows: every parenthesis ID operates on the
/// first counter, every bracket ID on the second, and neutral edges are
/// epsilon. Every arc must have its complement-labeled reverse arc after this
/// unary projection.
class AdaptiveSolver {
public:
  /// Compute the exact full component partition using K = 6n after optional
  /// quotient sparsification.
  AdaptiveResult solve(const Graph &graph,
                       const AdaptiveOptions &options = {}) const;

  /// Compute exactly the zero-configuration partition inside
  /// X_K = {(v,a,b) : min(a,b) <= K}. This direct form does not sparsify,
  /// because quotient lifting preserves full reachability but need not
  /// preserve membership in a caller-selected shallow region.
  AdaptiveResult solveShallow(const Graph &graph, std::size_t threshold,
                              const AdaptiveOptions &options = {}) const;
};

} // namespace lotus::cfl::unary_interleaved_dyck
