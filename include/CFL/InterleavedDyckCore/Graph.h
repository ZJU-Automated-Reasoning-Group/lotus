#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::interleaved_dyck {

using Vertex = std::int64_t;

enum class LabelKind {
  OpenParenthesis,
  CloseParenthesis,
  OpenBracket,
  CloseBracket,
  Neutral,
};

struct Label {
  LabelKind kind = LabelKind::Neutral;
  unsigned id = 0;

  static Label openParenthesis(unsigned id);
  static Label closeParenthesis(unsigned id);
  static Label openBracket(unsigned id);
  static Label closeBracket(unsigned id);
  static Label neutral();
  static Label parse(std::string_view text);

  std::string str() const;
  Label complement() const;

  bool operator==(const Label &other) const;
  bool operator!=(const Label &other) const { return !(*this == other); }
};

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
  bool containsVertex(Vertex vertex) const;
  bool empty() const { return vertices_.empty(); }

  static Graph parseDot(std::istream &input);
  static Graph parseDotFile(const std::string &path);

private:
  std::vector<Vertex> vertices_;
  std::vector<Edge> edges_;
  std::unordered_set<Vertex> vertex_set_;
  std::unordered_set<Edge, EdgeHash> edge_set_;
};

} // namespace lotus::cfl::interleaved_dyck
