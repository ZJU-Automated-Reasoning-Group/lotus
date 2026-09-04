#include "CFL/Classical/Core/Graph.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

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

bool endsWith(const std::string &value, const std::string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

std::string attributedLabel(const std::string &label,
                            const std::string &attribute) {
  if (!endsWith(label, "_i")) {
    return label;
  }
  if (attribute.empty()) {
    return label.substr(0, label.size() - 1) + '0';
  }
  if (!std::all_of(attribute.begin(), attribute.end(),
                   [](unsigned char c) { return std::isdigit(c) != 0; })) {
    throw std::invalid_argument("Invalid attributed graph label: " + label +
                                " " + attribute);
  }
  return label.substr(0, label.size() - 1) + attribute;
}

bool looksLikeDotFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open graph file: " + path);
  }
  for (std::string line; std::getline(input, line);) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    return startsWith(line, "digraph") ||
           line.find("->") != std::string::npos || line == "{";
  }
  return false;
}

std::string decodeDotIdentifier(std::string value) {
  value = trim(value);
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return value;
  }
  std::string decoded;
  decoded.reserve(value.size() - 2);
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    if (value[index] == '\\' && index + 2 < value.size()) {
      ++index;
    }
    decoded.push_back(value[index]);
  }
  return decoded;
}

} // namespace

LabeledGraph LabeledGraph::parseFromFile(const std::string &path,
                                         GraphMode mode) {
  return parseFromFile(path, GraphLoadOptions{mode, EdgeDirection::Plain});
}

LabeledGraph LabeledGraph::parseFromFile(const std::string &path,
                                         const GraphLoadOptions &options) {
  LabeledGraph graph;
  const auto dot_pos = path.find_last_of('.');
  const auto suffix =
      dot_pos == std::string::npos ? std::string() : path.substr(dot_pos + 1);
  if (suffix == "txt" || suffix == "peg" || suffix == "vfg" ||
      suffix == "graph") {
    graph.loadFromTextFile(path);
  } else if (suffix == "json") {
    graph.loadFromJsonFile(path);
  } else if (suffix == "dot" || looksLikeDotFile(path)) {
    graph.loadFromDotFile(path, options.mode);
  } else {
    graph.loadFromTextFile(path);
  }
  return graph.transformed(options.direction);
}

void LabeledGraph::writeTextFile(const std::string &path) const {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("Failed to open graph output file: " + path);
  }
  std::vector<std::size_t> sources(source_vertices_.begin(),
                                   source_vertices_.end());
  std::sort(sources.begin(), sources.end());
  for (std::size_t node : sources) {
    output << vertexName(node) << '\t' << vertexName(node) << "\tsrc\n";
  }
  std::vector<LabeledEdge> ordered = edges();
  std::sort(ordered.begin(), ordered.end(),
            [](const LabeledEdge &first, const LabeledEdge &second) {
              return std::tie(first.source, first.target, first.label) <
                     std::tie(second.source, second.target, second.label);
            });
  for (const LabeledEdge &edge : ordered) {
    output << vertexName(edge.source) << ',' << vertexName(edge.target) << ','
           << edge.label << '\n';
  }
}

std::string LabeledGraph::complementLabel(const std::string &label) {
  const auto attribute = label.find('_');
  std::string base = label.substr(0, attribute);
  const std::string suffix =
      attribute == std::string::npos ? "" : label.substr(attribute);
  constexpr const char *bar = "bar";
  if (base.size() >= 3 && base.compare(base.size() - 3, 3, bar) == 0) {
    base.erase(base.size() - 3);
  } else {
    base += bar;
  }
  return base + suffix;
}

LabeledGraph LabeledGraph::transformed(EdgeDirection direction) const {
  if (direction == EdgeDirection::Plain) {
    return *this;
  }

  LabeledGraph result;
  for (const std::string &vertex : vertices_) {
    result.addVertex(vertex);
  }
  for (std::size_t source : source_vertices_) {
    result.markSource(source);
  }
  for (const LabeledEdge &edge : edges()) {
    if (direction == EdgeDirection::Reverse) {
      result.addEdge(edge.target, edge.source, complementLabel(edge.label));
      continue;
    }
    result.addEdge(edge.source, edge.target, edge.label);
    result.addEdge(edge.target, edge.source, complementLabel(edge.label));
  }
  return result;
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
  reverse_adjacency_by_target_.emplace_back();
  ++mutation_version_;
  return id;
}

bool LabeledGraph::addEdge(const std::string &source, const std::string &target,
                           const std::string &label) {
  return addEdge(addVertex(source), addVertex(target), label);
}

bool LabeledGraph::addEdge(std::size_t source, std::size_t target,
                           const std::string &label) {
  if (source >= vertexCount() || target >= vertexCount()) {
    throw std::out_of_range("Graph edge endpoint is out of range");
  }
  auto &labels = adjacency_.at(source)[target];
  const auto [_, inserted] = labels.insert(label);
  if (!inserted) {
    return false;
  }

  label_pairs_[label].push_back({source, target});
  reverse_label_adjacency_[label][target].insert(source);
  reverse_adjacency_by_target_[target][label].insert(source);
  ++edge_count_;
  ++mutation_version_;
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

void LabeledGraph::markSource(std::size_t node) {
  if (node >= vertexCount()) {
    throw std::out_of_range("Source marker node is out of range");
  }
  if (source_vertices_.insert(node).second) {
    ++mutation_version_;
  }
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

void LabeledGraph::loadFromTextFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open graph file: " + path);
  }

  std::string line;
  while (std::getline(input, line)) {
    line = trim(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }

    const auto first = line.find(',');
    const auto second =
        line.find(',', first == std::string::npos ? first : first + 1);
    if (first != std::string::npos && second != std::string::npos) {
      addEdge(trim(line.substr(0, first)),
              trim(line.substr(first + 1, second - first - 1)),
              trim(line.substr(second + 1)));
      continue;
    }

    std::istringstream fields(line);
    std::vector<std::string> tokens;
    for (std::string token; fields >> token;) {
      tokens.push_back(std::move(token));
    }
    if (tokens.size() < 3 || tokens.size() > 4) {
      throw std::invalid_argument("Malformed graph line: " + line);
    }
    addVertex(tokens[0]);
    addVertex(tokens[1]);
    if (tokens[2] == "src") {
      markSource(vertexId(tokens[0]));
      continue;
    }
    addEdge(tokens[0], tokens[1],
            attributedLabel(tokens[2], tokens.size() == 4 ? tokens[3] : ""));
  }
}

void LabeledGraph::loadFromDotFile(const std::string &path, GraphMode mode) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open DOT graph file: " + path);
  }

  const std::string identifier = R"(("(?:\\.|[^"])*"|[A-Za-z0-9_.:-]+))";
  const std::regex edge_pattern(identifier + R"(\s*->\s*)" + identifier +
                                R"(\s*\[[^\]]*color\s*=\s*)" + identifier +
                                R"([^\]]*\])");
  const std::regex labeled_edge_pattern(
      identifier + R"(\s*->\s*)" + identifier + R"(\s*\[[^\]]*label\s*=\s*)" +
      identifier + R"([^\]]*\])");
  const std::regex node_pattern("^\\s*" + identifier +
                                R"(\s*(?:\[.*\])?;?\s*$)");

  std::string line;
  while (std::getline(input, line)) {
    std::smatch match;
    if (std::regex_search(line, match, labeled_edge_pattern)) {
      addEdge(decodeDotIdentifier(match[1].str()),
              decodeDotIdentifier(match[2].str()),
              decodeDotIdentifier(match[3].str()));
      continue;
    }
    if (std::regex_search(line, match, edge_pattern)) {
      const auto source = decodeDotIdentifier(match[1].str());
      const auto target = decodeDotIdentifier(match[2].str());
      const auto color = decodeDotIdentifier(match[3].str());

      addVertex(source);
      addVertex(target);

      if (mode == GraphMode::Plain) {
        addEdge(source, target, color);
        continue;
      }

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
      const auto node = decodeDotIdentifier(match[1].str());
      if (!node.empty() && node != "digraph" && node != "{") {
        addVertex(node);
      }
    }
  }
}

void LabeledGraph::loadFromJsonFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Failed to open JSON graph file: " + path);
  }
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  auto parsed = llvm::json::parse(text);
  if (!parsed) {
    throw std::invalid_argument("Invalid JSON graph: " +
                                llvm::toString(parsed.takeError()));
  }

  const llvm::json::Array *edges = parsed->getAsArray();
  bool has_node_section = false;
  if (const auto *root = parsed->getAsObject()) {
    const llvm::json::Array *nodes = root->getArray("nodes");
    if (!nodes) {
      nodes = root->getArray("vertices");
    }
    if (nodes) {
      has_node_section = true;
      for (const llvm::json::Value &value : *nodes) {
        if (const auto name = value.getAsString()) {
          addVertex(name->str());
          continue;
        }
        const auto *object = value.getAsObject();
        if (!object) {
          throw std::invalid_argument(
              "JSON graph node must be a string or contain id/name");
        }
        const auto name = object->getString("id");
        const auto fallback = object->getString("name");
        if (!name && !fallback) {
          throw std::invalid_argument(
              "JSON graph node must be a string or contain id/name");
        }
        addVertex((name ? *name : *fallback).str());
      }
    }
    edges = root->getArray("edges");
  }
  if (!edges) {
    if (has_node_section) {
      return;
    }
    throw std::invalid_argument(
        "JSON graph must be an edge array or contain an edges array");
  }
  for (const llvm::json::Value &value : *edges) {
    const auto *object = value.getAsObject();
    if (!object) {
      throw std::invalid_argument("JSON graph edge must be an object");
    }
    const auto source = object->getString("source");
    const auto target = object->getString("target");
    const auto label = object->getString("label");
    if (!source || !target || !label) {
      throw std::invalid_argument(
          "JSON graph edge requires string source, target, and label fields");
    }
    addEdge(source->str(), target->str(), label->str());
  }
}

} // namespace lotus::cfl::classical
