#include "CFL/Classical/Graph.h"

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>

namespace lotus::cfl::classical {
namespace {

std::string trim(const std::string &text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }

  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

bool startsWith(const std::string &value, const std::string &prefix) {
  return value.rfind(prefix, 0) == 0;
}

} // namespace

LabeledGraph LabeledGraph::parseFromFile(const std::string &path,
                                         GraphMode mode) {
  LabeledGraph graph;
  const auto dot_pos = path.find_last_of('.');
  const auto suffix =
      dot_pos == std::string::npos ? std::string() : path.substr(dot_pos + 1);
  if (suffix == "txt") {
    graph.loadFromTextFile(path);
  } else {
    graph.loadFromDotFile(path, mode);
  }
  return graph;
}

std::size_t LabeledGraph::addVertex(const std::string &name) {
  const auto it = vertex_ids_.find(name);
  if (it != vertex_ids_.end()) {
    return it->second;
  }

  const auto id = vertices_.size();
  vertices_.push_back(name);
  vertex_ids_.emplace(name, id);
  adjacency_.emplace_back();
  return id;
}

bool LabeledGraph::addEdge(const std::string &source, const std::string &target,
                           const std::string &label) {
  return addEdge(addVertex(source), addVertex(target), label);
}

bool LabeledGraph::addEdge(std::size_t source, std::size_t target,
                           const std::string &label) {
  auto &labels = adjacency_.at(source)[target];
  const auto [_, inserted] = labels.insert(label);
  if (!inserted) {
    return false;
  }

  label_pairs_[label].push_back({source, target});
  return true;
}

bool LabeledGraph::hasEdge(std::size_t source, std::size_t target,
                           const std::string &label) const {
  const auto out_it = adjacency_.at(source).find(target);
  if (out_it == adjacency_.at(source).end()) {
    return false;
  }
  return out_it->second.count(label) != 0;
}

std::size_t LabeledGraph::vertexId(const std::string &name) const {
  const auto it = vertex_ids_.find(name);
  if (it == vertex_ids_.end()) {
    throw std::out_of_range("Unknown vertex: " + name);
  }
  return it->second;
}

const std::string &LabeledGraph::vertexName(std::size_t id) const {
  return vertices_.at(id);
}

std::vector<LabeledEdge> LabeledGraph::edges() const {
  std::vector<LabeledEdge> all_edges;
  for (const auto &[label, pairs] : label_pairs_) {
    for (const auto &[source, target] : pairs) {
      all_edges.push_back({label, source, target});
    }
  }
  return all_edges;
}

const std::vector<std::pair<std::size_t, std::size_t>> &
LabeledGraph::edgesForLabel(const std::string &label) const {
  static const std::vector<std::pair<std::size_t, std::size_t>> empty;
  const auto it = label_pairs_.find(label);
  return it == label_pairs_.end() ? empty : it->second;
}

std::vector<std::pair<std::size_t, std::size_t>>
LabeledGraph::edgesForLabelCopy(const std::string &label) const {
  return edgesForLabel(label);
}

std::vector<std::size_t>
LabeledGraph::predecessorsForLabel(std::size_t target,
                                   const std::string &label) const {
  std::vector<std::size_t> predecessors;
  if (const auto it = label_pairs_.find(label); it != label_pairs_.end()) {
    for (const auto &[source, dst] : it->second) {
      if (dst == target) {
        predecessors.push_back(source);
      }
    }
  }
  return predecessors;
}

void LabeledGraph::loadFromTextFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open graph file: " + path);
  }

  std::string line;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    const auto first = line.find(',');
    const auto second =
        line.find(',', first == std::string::npos ? first : first + 1);
    if (first == std::string::npos || second == std::string::npos) {
      throw std::invalid_argument("Malformed graph line: " + line);
    }

    addEdge(trim(line.substr(0, first)),
            trim(line.substr(first + 1, second - first - 1)),
            trim(line.substr(second + 1)));
  }
}

void LabeledGraph::loadFromDotFile(const std::string &path, GraphMode mode) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open DOT graph file: " + path);
  }

  const std::regex edge_pattern(
      R"(([A-Za-z0-9_]+)\s*->\s*([A-Za-z0-9_]+)\s*\[.*color=([A-Za-z]+)[^\]]*\])");
  const std::regex node_pattern(R"(^\s*([A-Za-z0-9_]+)\s*(\[.*\])?;?\s*$)");

  std::string line;
  while (std::getline(input, line)) {
    std::smatch match;
    if (std::regex_search(line, match, edge_pattern)) {
      const auto source = match[1].str();
      const auto target = match[2].str();
      const auto color = match[3].str();

      addVertex(source);
      addVertex(target);

      if (mode == GraphMode::Matrix) {
        if (color == "red") {
          addEdge(target, source, "dbar");
          addEdge(source, target, "d");
        } else if (color == "black" || color == "purple") {
          addEdge(target, source, "abar");
          addEdge(source, target, "a");
        } else {
          addEdge(source, target, color);
        }
        continue;
      }

      if (startsWith(color, "red") || startsWith(color, "green")) {
        addEdge(target, source, "dbar");
        addEdge(source, target, "d");
      } else if (startsWith(color, "black")) {
        addEdge(target, source, "abar");
        addEdge(source, target, "a");
      } else if (startsWith(color, "blue")) {
        addEdge(target, source, "dbar");
        addEdge(source, target, "d");
      } else {
        addEdge(source, target, color);
      }
      continue;
    }

    if (std::regex_match(line, match, node_pattern)) {
      const auto node = trim(match[1].str());
      if (!node.empty() && node != "digraph" && node != "{") {
        addVertex(node);
      }
    }
  }
}

} // namespace lotus::cfl::classical
