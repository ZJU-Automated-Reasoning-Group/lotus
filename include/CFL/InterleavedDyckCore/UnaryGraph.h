#pragma once

#include "CFL/InterleavedDyckCore/BidirectedDyck.h"
#include "CFL/InterleavedDyckCore/Graph.h"

#include <cstddef>
#include <vector>

namespace lotus::cfl::interleaved_dyck {

enum class BidirectedInputPolicy {
  RequireBidirected,
  AddMissingReverseEdges,
};

enum class UnaryLabel : unsigned char {
  Epsilon,
  OpenFirst,
  CloseFirst,
  OpenSecond,
  CloseSecond,
};

UnaryLabel complement(UnaryLabel label);

struct UnaryEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  UnaryLabel label = UnaryLabel::Epsilon;

  bool operator==(const UnaryEdge &other) const;
};

struct UnaryGraph {
  std::size_t vertex_count = 0;
  std::vector<UnaryEdge> edges;
};

struct UnaryProjection {
  UnaryGraph graph;
  std::vector<Vertex> vertices;
  std::size_t original_arc_count = 0;
  std::size_t added_reverse_arcs = 0;
};

UnaryProjection projectToUnary(const Graph &graph,
                               BidirectedInputPolicy input_policy =
                                   BidirectedInputPolicy::RequireBidirected);

struct UnaryQuotient {
  UnaryGraph graph;
  std::vector<std::size_t> original_to_quotient;
  BidirectedDyckStats dyck;
};

/// Contract epsilon and forced ordinary-Dyck components of a fixed-alphabet
/// bidirected unary graph.
UnaryQuotient sparsifyUnaryGraph(const UnaryGraph &graph);

} // namespace lotus::cfl::interleaved_dyck
