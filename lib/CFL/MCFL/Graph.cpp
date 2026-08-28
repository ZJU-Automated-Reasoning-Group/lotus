#include "CFL/MCFL/Graph.h"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace lotus::cfl::mcfl {
namespace {

template <typename T> void hashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

Vertex parseVertex(std::string_view text, std::size_t line_number) {
  text = trim(text);
  Vertex vertex = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), vertex);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw std::runtime_error("invalid DOT vertex on line " +
                             std::to_string(line_number));
  }
  return vertex;
}

const std::vector<Edge> &emptyEdges() {
  static const std::vector<Edge> empty;
  return empty;
}

} // namespace

bool Edge::operator==(const Edge &other) const {
  return source == other.source && target == other.target &&
         label == other.label;
}

std::size_t EdgeHash::operator()(const Edge &edge) const {
  std::size_t seed = std::hash<Vertex>{}(edge.source);
  hashCombine(seed, edge.target);
  hashCombine(seed, edge.label);
  return seed;
}

bool Pair::operator==(const Pair &other) const {
  return source == other.source && target == other.target;
}

std::size_t PairHash::operator()(const Pair &pair) const {
  std::size_t seed = std::hash<Vertex>{}(pair.source);
  hashCombine(seed, pair.target);
  return seed;
}

std::size_t
Graph::VertexLabelKeyHash::operator()(const VertexLabelKey &key) const {
  std::size_t seed = std::hash<Vertex>{}(key.first);
  hashCombine(seed, key.second);
  return seed;
}

void Graph::addVertex(Vertex vertex) {
  if (vertex_set_.insert(vertex).second) {
    vertices_.push_back(vertex);
  }
}

bool Graph::addEdge(Vertex source, Vertex target, Label label) {
  addVertex(source);
  addVertex(target);
  Edge edge{source, target, std::move(label)};
  if (!edge_set_.insert(edge).second) {
    return false;
  }

  edges_.push_back(edge);
  label_edges_[edge.label].push_back(edge);
  incoming_[{target, edge.label}].push_back(edge);
  outgoing_[{source, edge.label}].push_back(edge);
  return true;
}

const std::vector<Edge> &Graph::edgesForLabel(std::string_view label) const {
  const auto found = label_edges_.find(std::string(label));
  return found == label_edges_.end() ? emptyEdges() : found->second;
}

const std::vector<Edge> &Graph::incoming(Vertex target,
                                         std::string_view label) const {
  const auto found = incoming_.find({target, std::string(label)});
  return found == incoming_.end() ? emptyEdges() : found->second;
}

const std::vector<Edge> &Graph::outgoing(Vertex source,
                                         std::string_view label) const {
  const auto found = outgoing_.find({source, std::string(label)});
  return found == outgoing_.end() ? emptyEdges() : found->second;
}

bool Graph::containsVertex(Vertex vertex) const {
  return vertex_set_.count(vertex) != 0U;
}

Graph Graph::parseDot(std::istream &input) {
  Graph graph;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::size_t arrow = line.find("->");
    if (arrow == std::string::npos) {
      continue;
    }

    const std::size_t attributes = line.find('[', arrow + 2);
    if (attributes == std::string::npos) {
      throw std::runtime_error("missing DOT attributes on line " +
                               std::to_string(line_number));
    }
    const Vertex source =
        parseVertex(std::string_view(line).substr(0, arrow), line_number);
    const Vertex target = parseVertex(
        std::string_view(line).substr(arrow + 2, attributes - arrow - 2),
        line_number);

    const std::size_t label_key = line.find("label", attributes);
    const std::size_t equals = label_key == std::string::npos
                                   ? std::string::npos
                                   : line.find('=', label_key + 5);
    const std::size_t quote = equals == std::string::npos
                                  ? std::string::npos
                                  : line.find('"', equals + 1);
    const std::size_t quote_end = quote == std::string::npos
                                      ? std::string::npos
                                      : line.find('"', quote + 1);
    if (quote_end == std::string::npos) {
      throw std::runtime_error("missing DOT edge label on line " +
                               std::to_string(line_number));
    }
    graph.addEdge(source, target,
                  line.substr(quote + 1, quote_end - quote - 1));
  }
  return graph;
}

Graph Graph::parseDotFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open MCFL DOT graph: " + path);
  }
  return parseDot(input);
}

} // namespace lotus::cfl::mcfl
