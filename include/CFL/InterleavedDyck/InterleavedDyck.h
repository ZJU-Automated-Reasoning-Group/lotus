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
  void addEdge(Vertex source, Vertex target, Label label);

  const std::vector<Vertex> &vertices() const { return vertices_; }
  const std::vector<Edge> &edges() const { return edges_; }
  bool empty() const { return edges_.empty(); }

  static Graph parseDot(std::istream &input);
  static Graph parseDotFile(const std::string &path);

private:
  std::vector<Vertex> vertices_;
  std::vector<Edge> edges_;
  std::unordered_set<Vertex> vertex_set_;
  std::unordered_set<Edge, EdgeHash> edge_set_;
};

enum class Alphabet { Parenthesis, Bracket };
enum class GrammarStrength { Classic, Parity };
enum class BenchmarkKind { Taint, ValueFlow };

struct Options {
  /// The prototype and paper use two parity groups. Supported values are 1-4.
  unsigned parity_groups = 2;
  /// On-demand refinement can be expensive because it checks unknown pairs
  /// separately. It is enabled by default to reproduce the full pipeline.
  bool run_on_demand = true;
};

struct ApproximationResult {
  PairSet regularization;
  PairSet intersection;
  PairSet underapproximation;
  PairSet mutual_refinement;
  PairSet stronger_grammar;
  PairSet on_demand;
};

class Solver {
public:
  /// Runs one projected Dyck grammar. Edges in the other alphabet are treated
  /// as unconstrained terminals, as in the original approximation.
  PairSet
  projectedReachability(const Graph &graph, Alphabet balanced_alphabet,
                        GrammarStrength strength = GrammarStrength::Classic,
                        unsigned parity_groups = 2) const;

  /// Intersects the two independently witnessed projected reachability sets.
  PairSet intersection(const Graph &graph,
                       GrammarStrength strength = GrammarStrength::Classic,
                       unsigned parity_groups = 2) const;

  /// Recognizes the Dyck language over the union of both alphabets. This is a
  /// sound underapproximation of interleaved-Dyck reachability.
  PairSet underapproximation(const Graph &graph) const;

  /// Alternates the two projected analyses and retains only graph edges used
  /// by their derivations until the edge set stabilizes.
  PairSet
  mutualRefinement(const Graph &graph,
                   GrammarStrength strength = GrammarStrength::Classic,
                   unsigned parity_groups = 2,
                   BenchmarkKind benchmark = BenchmarkKind::Taint) const;

  /// Reproduces the staged benchmark pipeline from the reference artifact.
  ApproximationResult analyze(const Graph &graph,
                              BenchmarkKind benchmark = BenchmarkKind::Taint,
                              const Options &options = {}) const;
};

} // namespace lotus::cfl::interleaved_dyck
