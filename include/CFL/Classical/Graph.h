#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {

enum class GraphMode {
  Matrix,
  PAGMatrix,
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

  std::size_t addVertex(const std::string &name);
  bool addEdge(const std::string &source, const std::string &target,
               const std::string &label);
  bool addEdge(std::size_t source, std::size_t target,
               const std::string &label);
  bool hasEdge(std::size_t source, std::size_t target,
               const std::string &label) const;

  std::size_t vertexId(const std::string &name) const;
  const std::string &vertexName(std::size_t id) const;
  std::size_t vertexCount() const { return vertices_.size(); }
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
  std::vector<std::size_t> predecessorsForLabel(std::size_t target,
                                                const std::string &label) const;

private:
  void loadFromTextFile(const std::string &path);
  void loadFromDotFile(const std::string &path, GraphMode mode);

  std::vector<std::string> vertices_;
  std::unordered_map<std::string, std::size_t> vertex_ids_;
  std::vector<std::unordered_map<std::size_t, std::unordered_set<std::string>>>
      adjacency_;
  std::unordered_map<std::string,
                     std::vector<std::pair<std::size_t, std::size_t>>>
      label_pairs_;
};

} // namespace lotus::cfl::classical
