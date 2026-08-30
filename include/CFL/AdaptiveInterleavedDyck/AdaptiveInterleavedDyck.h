#pragma once

#include "CFL/InterleavedDyckCore/Graph.h"

#include <cstddef>
#include <unordered_map>

namespace lotus::cfl::adaptive_interleaved_dyck {

using interleaved_dyck::Graph;
using interleaved_dyck::Vertex;

/// Construction statistics for adaptive unary interleaved-Dyck reachability.
struct AdaptiveInterleavedStats {
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
};

/// A component partition of the input vertices.
class AdaptiveInterleavedResult {
public:
  const std::unordered_map<Vertex, std::size_t> &components() const {
    return components_;
  }
  const AdaptiveInterleavedStats &stats() const { return stats_; }

  std::size_t component(Vertex vertex) const;
  bool connected(Vertex first, Vertex second) const;

private:
  friend class AdaptiveInterleavedDyckSolver;

  std::unordered_map<Vertex, std::size_t> components_;
  AdaptiveInterleavedStats stats_;
};

enum class BidirectedInputPolicy {
  /// Reject an input with any missing complement-labeled reverse arc. Results
  /// are exact for the original graph.
  RequireBidirected,
  /// Add every missing complement-labeled reverse arc. Results are exact for
  /// the symmetrized graph and a sound overapproximation for the original
  /// directed graph.
  AddMissingReverseEdges,
};

struct AdaptiveInterleavedOptions {
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
class AdaptiveInterleavedDyckSolver {
public:
  /// Compute the exact full component partition using K = 6n after optional
  /// quotient sparsification.
  AdaptiveInterleavedResult
  solve(const Graph &graph,
        const AdaptiveInterleavedOptions &options = {}) const;

  /// Compute exactly the zero-configuration partition inside
  /// X_K = {(v,a,b) : min(a,b) <= K}. This direct form does not sparsify,
  /// because quotient lifting preserves full reachability but need not
  /// preserve membership in a caller-selected shallow region.
  AdaptiveInterleavedResult
  solveShallow(const Graph &graph, std::size_t threshold,
               const AdaptiveInterleavedOptions &options = {}) const;
};

} // namespace lotus::cfl::adaptive_interleaved_dyck
