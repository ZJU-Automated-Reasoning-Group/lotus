#include "CFL/InterleavedDyckCore/UnaryGraph.h"

#include "CFL/InterleavedDyckCore/BidirectedDyck.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::interleaved_dyck {
namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

template <typename T> void hashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

UnaryLabel projectLabel(const Label &label) {
  switch (label.kind) {
  case LabelKind::Neutral:
    return UnaryLabel::Epsilon;
  case LabelKind::OpenParenthesis:
    return UnaryLabel::OpenFirst;
  case LabelKind::CloseParenthesis:
    return UnaryLabel::CloseFirst;
  case LabelKind::OpenBracket:
    return UnaryLabel::OpenSecond;
  case LabelKind::CloseBracket:
    return UnaryLabel::CloseSecond;
  }
  throw std::logic_error("invalid interleaved-Dyck label kind");
}

struct UnaryEdgeHash {
  std::size_t operator()(const UnaryEdge &edge) const {
    std::size_t seed = std::hash<std::size_t>{}(edge.source);
    hashCombine(seed, edge.target);
    hashCombine(seed, static_cast<unsigned char>(edge.label));
    return seed;
  }
};

} // namespace

UnaryLabel complement(UnaryLabel label) {
  switch (label) {
  case UnaryLabel::Epsilon:
    return UnaryLabel::Epsilon;
  case UnaryLabel::OpenFirst:
    return UnaryLabel::CloseFirst;
  case UnaryLabel::CloseFirst:
    return UnaryLabel::OpenFirst;
  case UnaryLabel::OpenSecond:
    return UnaryLabel::CloseSecond;
  case UnaryLabel::CloseSecond:
    return UnaryLabel::OpenSecond;
  }
  throw std::logic_error("invalid unary interleaved-Dyck label");
}

bool UnaryEdge::operator==(const UnaryEdge &other) const {
  return source == other.source && target == other.target &&
         label == other.label;
}

UnaryProjection projectToUnary(const Graph &input,
                               BidirectedInputPolicy input_policy) {
  UnaryProjection result;
  result.vertices = input.vertices();
  result.graph.vertex_count = result.vertices.size();

  std::unordered_map<Vertex, std::size_t> indices;
  indices.reserve(result.vertices.size());
  for (std::size_t i = 0; i < result.vertices.size(); ++i) {
    indices.emplace(result.vertices[i], i);
  }

  std::unordered_set<UnaryEdge, UnaryEdgeHash> unique;
  unique.reserve(input.edges().size());
  for (const Edge &edge : input.edges()) {
    unique.insert({indices.at(edge.source), indices.at(edge.target),
                   projectLabel(edge.label)});
  }
  result.original_arc_count = unique.size();

  const std::vector<UnaryEdge> original_edges(unique.begin(), unique.end());
  for (const UnaryEdge &edge : original_edges) {
    const UnaryEdge reverse{edge.target, edge.source, complement(edge.label)};
    if (unique.count(reverse) != 0U) {
      continue;
    }
    if (input_policy == BidirectedInputPolicy::RequireBidirected) {
      throw std::invalid_argument(
          "unary interleaved-Dyck reachability requires a bidirected graph; "
          "missing reverse arc for " +
          std::to_string(result.vertices[edge.source]) + " -> " +
          std::to_string(result.vertices[edge.target]));
    }
    unique.insert(reverse);
    ++result.added_reverse_arcs;
  }
  result.graph.edges.assign(unique.begin(), unique.end());
  return result;
}

UnaryQuotient sparsifyUnaryGraph(const UnaryGraph &graph) {
  std::vector<StatePair> epsilon_edges;
  std::vector<LabeledStateEdge> closing_edges;
  epsilon_edges.reserve(graph.edges.size());
  closing_edges.reserve(graph.edges.size() / 2);
  for (const UnaryEdge &edge : graph.edges) {
    switch (edge.label) {
    case UnaryLabel::Epsilon:
      epsilon_edges.push_back({edge.source, edge.target});
      break;
    case UnaryLabel::CloseFirst:
      closing_edges.push_back({edge.source, edge.target, 0});
      break;
    case UnaryLabel::CloseSecond:
      closing_edges.push_back({edge.source, edge.target, 1});
      break;
    case UnaryLabel::OpenFirst:
    case UnaryLabel::OpenSecond:
      break;
    }
  }

  const BidirectedDyckResult dyck = BidirectedDyckComponentSolver{}.solve(
      graph.vertex_count, 2, epsilon_edges, closing_edges);
  const std::vector<std::size_t> &components = dyck.component;

  UnaryQuotient result;
  result.dyck = dyck.stats;
  result.original_to_quotient.resize(graph.vertex_count);
  std::vector<std::size_t> quotient_indices(graph.vertex_count, kNone);
  std::size_t quotient_count = 0;
  for (std::size_t vertex = 0; vertex < graph.vertex_count; ++vertex) {
    const std::size_t component = components[vertex];
    if (quotient_indices[component] == kNone) {
      quotient_indices[component] = quotient_count++;
    }
    result.original_to_quotient[vertex] = quotient_indices[component];
  }
  result.graph.vertex_count = quotient_count;

  std::vector<std::array<std::size_t, 2>> close_targets(quotient_count);
  for (auto &targets : close_targets) {
    targets.fill(kNone);
  }
  for (const UnaryEdge &edge : graph.edges) {
    std::size_t label = 0;
    if (edge.label == UnaryLabel::CloseSecond) {
      label = 1;
    } else if (edge.label != UnaryLabel::CloseFirst) {
      continue;
    }
    const std::size_t source = result.original_to_quotient[edge.source];
    const std::size_t target = result.original_to_quotient[edge.target];
    std::size_t &stored = close_targets[source][label];
    if (stored == kNone) {
      stored = target;
    } else if (stored != target) {
      throw std::logic_error(
          "sparsified component has two closing targets for one label");
    }
  }
  for (std::size_t source = 0; source < quotient_count; ++source) {
    for (std::size_t label = 0; label < 2; ++label) {
      const std::size_t target = close_targets[source][label];
      if (target == kNone) {
        continue;
      }
      const UnaryLabel close =
          label == 0 ? UnaryLabel::CloseFirst : UnaryLabel::CloseSecond;
      result.graph.edges.push_back({source, target, close});
      result.graph.edges.push_back({target, source, complement(close)});
    }
  }
  return result;
}

} // namespace lotus::cfl::interleaved_dyck
