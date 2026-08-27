#include "CFL/InterleavedDyck/InterleavedDyck.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>

namespace lotus::cfl::interleaved_dyck {
namespace {

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

Vertex parseVertex(std::string_view text, std::size_t line_number) {
  text = trim(text);
  Vertex vertex = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), vertex);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::runtime_error("invalid DOT vertex on line " +
                             std::to_string(line_number));
  }
  return vertex;
}

unsigned parseLabelId(std::string_view text) {
  unsigned id = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), id);
  if (text.empty() || result.ec != std::errc{} ||
      result.ptr != text.data() + text.size()) {
    throw std::invalid_argument("invalid interleaved-Dyck label id: " +
                                std::string(text));
  }
  return id;
}

} // namespace

Label Label::openParenthesis(unsigned id) {
  return {LabelKind::OpenParenthesis, id};
}

Label Label::closeParenthesis(unsigned id) {
  return {LabelKind::CloseParenthesis, id};
}

Label Label::openBracket(unsigned id) { return {LabelKind::OpenBracket, id}; }

Label Label::closeBracket(unsigned id) { return {LabelKind::CloseBracket, id}; }

Label Label::neutral() { return {LabelKind::Neutral, 0}; }

Label Label::parse(std::string_view text) {
  text = trim(text);
  if (text == "normal") {
    return neutral();
  }
  if (text.size() <= 4 || text.substr(2, 2) != "--") {
    throw std::invalid_argument("unknown interleaved-Dyck label: " +
                                std::string(text));
  }

  const unsigned id = parseLabelId(text.substr(4));
  const auto prefix = text.substr(0, 2);
  if (prefix == "op") {
    return openParenthesis(id);
  }
  if (prefix == "cp") {
    return closeParenthesis(id);
  }
  if (prefix == "ob") {
    return openBracket(id);
  }
  if (prefix == "cb") {
    return closeBracket(id);
  }
  throw std::invalid_argument("unknown interleaved-Dyck label: " +
                              std::string(text));
}

std::string Label::str() const {
  switch (kind) {
  case LabelKind::OpenParenthesis:
    return "op--" + std::to_string(id);
  case LabelKind::CloseParenthesis:
    return "cp--" + std::to_string(id);
  case LabelKind::OpenBracket:
    return "ob--" + std::to_string(id);
  case LabelKind::CloseBracket:
    return "cb--" + std::to_string(id);
  case LabelKind::Neutral:
    return "normal";
  }
  throw std::logic_error("unhandled interleaved-Dyck label kind");
}

bool Label::operator==(const Label &other) const {
  return kind == other.kind && id == other.id;
}

bool Edge::operator==(const Edge &other) const {
  return source == other.source && target == other.target &&
         label == other.label;
}

std::size_t EdgeHash::operator()(const Edge &edge) const {
  std::size_t seed = std::hash<Vertex>{}(edge.source);
  seed ^= std::hash<Vertex>{}(edge.target) + 0x9e3779b9U + (seed << 6U) +
          (seed >> 2U);
  const auto kind = static_cast<unsigned>(edge.label.kind);
  seed ^=
      std::hash<unsigned>{}(kind) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
  seed ^= std::hash<unsigned>{}(edge.label.id) + 0x9e3779b9U + (seed << 6U) +
          (seed >> 2U);
  return seed;
}

bool Pair::operator==(const Pair &other) const {
  return source == other.source && target == other.target;
}

std::size_t PairHash::operator()(const Pair &pair) const {
  std::size_t seed = std::hash<Vertex>{}(pair.source);
  seed ^= std::hash<Vertex>{}(pair.target) + 0x9e3779b9U + (seed << 6U) +
          (seed >> 2U);
  return seed;
}

void Graph::addVertex(Vertex vertex) {
  if (vertex_set_.insert(vertex).second) {
    vertices_.push_back(vertex);
  }
}

void Graph::addEdge(Vertex source, Vertex target, Label label) {
  addVertex(source);
  addVertex(target);
  const Edge candidate{source, target, label};
  if (edge_set_.insert(candidate).second) {
    edges_.push_back(candidate);
  }
}

Graph Graph::parseDot(std::istream &input) {
  Graph graph;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const auto arrow = line.find("->");
    if (arrow == std::string::npos) {
      continue;
    }
    const auto bracket = line.find('[', arrow + 2);
    if (bracket == std::string::npos) {
      throw std::runtime_error("missing DOT edge attributes on line " +
                               std::to_string(line_number));
    }

    const Vertex source =
        parseVertex(std::string_view(line).substr(0, arrow), line_number);
    const Vertex target = parseVertex(
        std::string_view(line).substr(arrow + 2, bracket - arrow - 2),
        line_number);

    const auto label_key = line.find("label", bracket);
    const auto equals = label_key == std::string::npos
                            ? std::string::npos
                            : line.find('=', label_key + 5);
    const auto quote = equals == std::string::npos ? std::string::npos
                                                   : line.find('"', equals + 1);
    const auto quote_end = quote == std::string::npos
                               ? std::string::npos
                               : line.find('"', quote + 1);
    if (quote_end == std::string::npos) {
      throw std::runtime_error("missing DOT edge label on line " +
                               std::to_string(line_number));
    }

    const Label label = Label::parse(
        std::string_view(line).substr(quote + 1, quote_end - quote - 1));
    graph.addEdge(source, target, label);
  }
  return graph;
}

Graph Graph::parseDotFile(const std::string &path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open interleaved-Dyck DOT graph: " + path);
  }
  return parseDot(input);
}

} // namespace lotus::cfl::interleaved_dyck
