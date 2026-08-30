#include "CFL/UnaryInterleavedDyck/FixedCounter.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lotus::cfl::unary_interleaved_dyck {
namespace {

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

std::size_t checkedAdd(std::size_t left, std::size_t right,
                       const char *description) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    throw std::overflow_error(std::string(description) + " is too large");
  }
  return left + right;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right,
                            const char *description) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error(std::string(description) + " is too large");
  }
  return left * right;
}

std::size_t counterBound(std::size_t vertex_count) {
  const std::size_t square = checkedMultiply(vertex_count, vertex_count,
                                             "fixed-counter counter bound");
  const std::size_t quadratic =
      checkedMultiply(18, square, "fixed-counter counter bound");
  const std::size_t linear =
      checkedMultiply(6, vertex_count, "fixed-counter counter bound");
  return checkedAdd(quadratic, linear, "fixed-counter counter bound");
}

std::size_t controlState(std::size_t vertex, std::size_t counter,
                         std::size_t width) {
  return vertex * width + counter;
}

struct FixedCounterInstance {
  std::size_t state_count = 0;
  std::size_t translated_arcs = 0;
  std::vector<interleaved_dyck::StatePair> epsilon_edges;
  std::vector<interleaved_dyck::LabeledStateEdge> closing_edges;
};

FixedCounterInstance
buildFixedCounterInstance(const interleaved_dyck::UnaryGraph &graph,
                          std::size_t bound) {
  FixedCounterInstance result;
  const std::size_t width = checkedAdd(bound, 1, "fixed-counter counter width");
  result.state_count =
      checkedMultiply(graph.vertex_count, width, "fixed-counter state space");

  for (const interleaved_dyck::UnaryEdge &edge : graph.edges) {
    switch (edge.label) {
    case interleaved_dyck::UnaryLabel::Epsilon:
      for (std::size_t value = 0; value < width; ++value) {
        result.epsilon_edges.push_back(
            {controlState(edge.source, value, width),
             controlState(edge.target, value, width)});
        ++result.translated_arcs;
      }
      break;
    case interleaved_dyck::UnaryLabel::OpenFirst:
      result.translated_arcs = checkedAdd(result.translated_arcs, width,
                                          "fixed-counter translated arcs");
      break;
    case interleaved_dyck::UnaryLabel::CloseFirst:
      for (std::size_t value = 0; value < width; ++value) {
        result.closing_edges.push_back({controlState(edge.source, value, width),
                                        controlState(edge.target, value, width),
                                        0});
        ++result.translated_arcs;
      }
      break;
    case interleaved_dyck::UnaryLabel::OpenSecond:
      for (std::size_t value = 0; value < bound; ++value) {
        result.epsilon_edges.push_back(
            {controlState(edge.source, value, width),
             controlState(edge.target, value + 1, width)});
        ++result.translated_arcs;
      }
      break;
    case interleaved_dyck::UnaryLabel::CloseSecond:
      for (std::size_t value = 1; value < width; ++value) {
        result.epsilon_edges.push_back(
            {controlState(edge.source, value, width),
             controlState(edge.target, value - 1, width)});
        ++result.translated_arcs;
      }
      break;
    }
  }
  return result;
}

struct ComputedResult {
  std::unordered_map<Vertex, std::size_t> components;
  FixedCounterStats stats;
};

ComputedResult makeResult(const interleaved_dyck::UnaryProjection &projection,
                          const interleaved_dyck::UnaryGraph &processed,
                          const std::vector<std::size_t> &original_to_processed,
                          bool was_sparsified) {
  ComputedResult result;
  result.stats.input_vertices = projection.graph.vertex_count;
  result.stats.input_arcs = projection.original_arc_count;
  result.stats.quotient_vertices = processed.vertex_count;
  result.stats.quotient_arcs = processed.edges.size();
  result.stats.added_reverse_arcs = projection.added_reverse_arcs;
  result.stats.input_was_bidirected = projection.added_reverse_arcs == 0;
  result.stats.overapproximates_original = projection.added_reverse_arcs != 0;
  result.stats.sparsified = was_sparsified;

  if (processed.vertex_count == 0) {
    return result;
  }

  const std::size_t bound = counterBound(processed.vertex_count);
  const std::size_t width = checkedAdd(bound, 1, "fixed-counter counter width");
  const FixedCounterInstance instance =
      buildFixedCounterInstance(processed, bound);
  const interleaved_dyck::BidirectedDyckResult dyck =
      interleaved_dyck::BidirectedDyckComponentSolver{}.solve(
          instance.state_count, 1, instance.epsilon_edges,
          instance.closing_edges);

  result.stats.counter_bound = bound;
  result.stats.control_states = instance.state_count;
  result.stats.translated_arcs = instance.translated_arcs;
  result.stats.epsilon_edges = instance.epsilon_edges.size();
  result.stats.closing_edges = instance.closing_edges.size();
  result.stats.dyck = dyck.stats;

  std::vector<std::size_t> identifiers(instance.state_count, kNone);
  std::size_t identifier_count = 0;
  result.components.reserve(projection.vertices.size());
  for (std::size_t vertex = 0; vertex < projection.vertices.size(); ++vertex) {
    const std::size_t processed_vertex = original_to_processed[vertex];
    const std::size_t component =
        dyck.component[controlState(processed_vertex, 0, width)];
    if (identifiers[component] == kNone) {
      identifiers[component] = identifier_count++;
    }
    result.components.emplace(projection.vertices[vertex],
                              identifiers[component]);
  }
  return result;
}

} // namespace

std::size_t FixedCounterResult::component(Vertex vertex) const {
  const auto found = components_.find(vertex);
  if (found == components_.end()) {
    throw std::out_of_range("unknown unary interleaved-Dyck vertex");
  }
  return found->second;
}

bool FixedCounterResult::connected(Vertex first, Vertex second) const {
  return component(first) == component(second);
}

FixedCounterResult
FixedCounterSolver::solve(const Graph &graph,
                          const FixedCounterOptions &options) const {
  const interleaved_dyck::UnaryProjection projection =
      interleaved_dyck::projectToUnary(graph, options.input_policy);
  ComputedResult computed;
  if (options.sparsify) {
    const interleaved_dyck::UnaryQuotient quotient =
        interleaved_dyck::sparsifyUnaryGraph(projection.graph);
    computed = makeResult(projection, quotient.graph,
                          quotient.original_to_quotient, true);
    computed.stats.quotient_dyck = quotient.dyck;
  } else {
    std::vector<std::size_t> identity(projection.graph.vertex_count);
    for (std::size_t vertex = 0; vertex < identity.size(); ++vertex) {
      identity[vertex] = vertex;
    }
    computed = makeResult(projection, projection.graph, identity, false);
  }

  FixedCounterResult result;
  result.components_ = std::move(computed.components);
  result.stats_ = computed.stats;
  return result;
}

} // namespace lotus::cfl::unary_interleaved_dyck
