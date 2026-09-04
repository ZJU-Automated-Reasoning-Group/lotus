#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {

enum class GraphMode {
  Plain,
  Matrix,
  PAGMatrix,
};

enum class EdgeDirection {
  Plain,
  Reverse,
  Bidirectional,
};

struct GraphLoadOptions {
  GraphMode mode = GraphMode::Plain;
  EdgeDirection direction = EdgeDirection::Plain;
};

struct LabeledEdge {
  std::string label;
  std::size_t source;
  std::size_t target;
};

class LabeledGraph {
public:
  static LabeledGraph parseFromFile(const std::string &path,
                                    GraphMode mode = GraphMode::Matrix);
  static LabeledGraph parseFromFile(const std::string &path,
                                    const GraphLoadOptions &options);
  void writeTextFile(const std::string &path) const;

  /// Return a graph with an explicit direction transform. Reverse labels use
  /// the conventional x/xbar and call_i/callbar_i pairing.
  LabeledGraph transformed(EdgeDirection direction) const;
  static std::string complementLabel(const std::string &label);

  std::size_t addVertex(const std::string &name);
  bool addEdge(const std::string &source, const std::string &target,
               const std::string &label);
  bool addEdge(std::size_t source, std::size_t target,
               const std::string &label);
  bool hasEdge(std::size_t source, std::size_t target,
               const std::string &label) const;
  void markSource(std::size_t node);
  bool isSource(std::size_t node) const {
    return source_vertices_.count(node) != 0;
  }

  std::size_t vertexId(const std::string &name) const;
  const std::string &vertexName(std::size_t id) const;
  std::size_t vertexCount() const { return vertices_.size(); }
  std::size_t edgeCount() const { return edge_count_; }
  std::uint64_t mutationVersion() const { return mutation_version_; }
  const std::vector<std::string> &vertices() const { return vertices_; }
  std::vector<LabeledEdge> edges() const;
  const std::unordered_map<std::string,
                           std::vector<std::pair<std::size_t, std::size_t>>> &
  symbolPairs() const {
    return label_pairs_;
  }
  const std::vector<std::pair<std::size_t, std::size_t>> &
  edgesForLabel(const std::string &label) const;
  std::vector<std::pair<std::size_t, std::size_t>>
  edgesForLabelCopy(const std::string &label) const;
  template <typename Visitor>
  void forEachPredecessorForLabel(const std::string &label, std::size_t target,
                                  Visitor &&visitor) const {
    const auto label_it = reverse_label_adjacency_.find(label);
    if (label_it == reverse_label_adjacency_.end()) {
      return;
    }
    const auto target_it = label_it->second.find(target);
    if (target_it == label_it->second.end()) {
      return;
    }
    for (std::size_t source : target_it->second) {
      visitor(source);
    }
  }

  template <typename Visitor>
  void forEachIncomingEdge(std::size_t target, Visitor &&visitor) const {
    for (const auto &[label, sources] :
         reverse_adjacency_by_target_.at(target)) {
      for (std::size_t source : sources) {
        visitor(label, source);
      }
    }
  }

  template <typename NodeRange, typename EdgeRange, typename NodeName,
            typename EdgeSource, typename EdgeTarget, typename EdgeLabel>
  static LabeledGraph build(const NodeRange &nodes, const EdgeRange &edges,
                            NodeName node_name, EdgeSource edge_source,
                            EdgeTarget edge_target, EdgeLabel edge_label) {
    LabeledGraph graph;
    for (const auto &node : nodes) {
      graph.addVertex(node_name(node));
    }
    for (const auto &edge : edges) {
      graph.addEdge(edge_source(edge), edge_target(edge), edge_label(edge));
    }
    return graph;
  }

private:
  void loadFromTextFile(const std::string &path);
  void loadFromDotFile(const std::string &path, GraphMode mode);
  void loadFromJsonFile(const std::string &path);

  std::vector<std::string> vertices_;
  std::unordered_map<std::string, std::size_t> vertex_ids_;
  std::unordered_set<std::size_t> source_vertices_;
  std::vector<std::unordered_map<std::size_t, std::unordered_set<std::string>>>
      adjacency_;
  std::unordered_map<std::string,
                     std::vector<std::pair<std::size_t, std::size_t>>>
      label_pairs_;
  std::unordered_map<
      std::string,
      std::unordered_map<std::size_t, std::unordered_set<std::size_t>>>
      reverse_label_adjacency_;
  std::vector<std::unordered_map<std::string, std::unordered_set<std::size_t>>>
      reverse_adjacency_by_target_;
  std::size_t edge_count_ = 0;
  std::uint64_t mutation_version_ = 0;
};

} // namespace lotus::cfl::classical
