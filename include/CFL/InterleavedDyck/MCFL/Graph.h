#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::interleaved_dyck::mcfl {

using Vertex = std::int64_t;
using Label = std::string;

inline constexpr std::string_view kEpsilonLabel = "";

struct Edge {
  Vertex source = 0;
  Vertex target = 0;
  Label label;

  bool operator==(const Edge &other) const;
};

struct EdgeHash {
  std::size_t operator()(const Edge &edge) const;
};

struct Pair {
  Vertex source = 0;
  Vertex target = 0;

  bool operator==(const Pair &other) const;
  bool operator!=(const Pair &other) const { return !(*this == other); }
};

struct PairHash {
  std::size_t operator()(const Pair &pair) const;
};

using PairSet = std::unordered_set<Pair, PairHash>;

class Graph {
public:
  void addVertex(Vertex vertex);
  bool addEdge(Vertex source, Vertex target, Label label);

  const std::vector<Vertex> &vertices() const { return vertices_; }
  const std::vector<Edge> &edges() const { return edges_; }
  const std::vector<Edge> &edgesForLabel(std::string_view label) const;
  const std::vector<Edge> &incoming(Vertex target,
                                    std::string_view label) const;
  const std::vector<Edge> &outgoing(Vertex source,
                                    std::string_view label) const;

  bool containsVertex(Vertex vertex) const;
  bool empty() const { return vertices_.empty(); }

  static Graph parseDot(std::istream &input);
  static Graph parseDotFile(const std::string &path);

private:
  using VertexLabelKey = std::pair<Vertex, Label>;

  struct VertexLabelKeyHash {
    std::size_t operator()(const VertexLabelKey &key) const;
  };

  std::vector<Vertex> vertices_;
  std::vector<Edge> edges_;
  std::unordered_set<Vertex> vertex_set_;
  std::unordered_set<Edge, EdgeHash> edge_set_;
  std::unordered_map<Label, std::vector<Edge>> label_edges_;
  std::unordered_map<VertexLabelKey, std::vector<Edge>, VertexLabelKeyHash>
      incoming_;
  std::unordered_map<VertexLabelKey, std::vector<Edge>, VertexLabelKeyHash>
      outgoing_;
};

} // namespace lotus::cfl::interleaved_dyck::mcfl
