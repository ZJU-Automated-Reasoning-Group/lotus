#include "CFL/InterleavedDyck/Unary/Adaptive.h"

#include "CFL/InterleavedDyck/Core/BidirectedDyck.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::interleaved_dyck::unary {
namespace {

using interleaved_dyck::BidirectedDyckComponentSolver;
using interleaved_dyck::LabeledStateEdge;
using interleaved_dyck::projectToUnary;
using interleaved_dyck::sparsifyUnaryGraph;
using interleaved_dyck::StatePair;
using interleaved_dyck::UnaryEdge;
using interleaved_dyck::UnaryGraph;
using interleaved_dyck::UnaryLabel;
using interleaved_dyck::UnaryProjection;
using interleaved_dyck::UnaryQuotient;

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

std::size_t checkedAdd(std::size_t left, std::size_t right,
                       std::string_view description) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    throw std::overflow_error(std::string(description) + " is too large");
  }
  return left + right;
}

std::size_t checkedMultiply(std::size_t left, std::size_t right,
                            std::string_view description) {
  if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
    throw std::overflow_error(std::string(description) + " is too large");
  }
  return left * right;
}

class DisjointSets {
public:
  explicit DisjointSets(std::size_t size) : parent_(size), size_(size, 1) {
    for (std::size_t i = 0; i < size; ++i) {
      parent_[i] = i;
    }
  }

  std::size_t find(std::size_t element) {
    std::size_t &parent = parent_.at(element);
    if (parent != element) {
      parent = find(parent);
    }
    return parent;
  }

  void join(std::size_t first, std::size_t second) {
    first = find(first);
    second = find(second);
    if (first == second) {
      return;
    }
    if (size_[first] < size_[second]) {
      std::swap(first, second);
    }
    parent_[second] = first;
    size_[first] += size_[second];
  }

private:
  std::vector<std::size_t> parent_;
  std::vector<std::size_t> size_;
};

struct OneCounterArm {
  std::size_t control_states = 0;
  std::size_t arc_count = 0;
  std::vector<StatePair> epsilon_edges;
  std::vector<LabeledStateEdge> closing_edges;
};

struct Arms {
  OneCounterArm vertical;
  OneCounterArm horizontal;
  std::size_t width = 0;
};

std::size_t controlState(std::size_t vertex, std::size_t value,
                         std::size_t width) {
  return vertex * width + value;
}

Arms buildArms(const UnaryGraph &graph, std::size_t threshold) {
  Arms result;
  result.width = checkedAdd(threshold, 1, "adaptive threshold");
  const std::size_t state_count =
      checkedMultiply(graph.vertex_count, result.width, "arm state space");
  result.vertical.control_states = state_count;
  result.horizontal.control_states = state_count;

  for (const UnaryEdge &edge : graph.edges) {
    switch (edge.label) {
    case UnaryLabel::Epsilon:
      for (std::size_t value = 0; value < result.width; ++value) {
        result.vertical.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value, result.width)});
        result.horizontal.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value, result.width)});
        ++result.vertical.arc_count;
        ++result.horizontal.arc_count;
      }
      break;

    case UnaryLabel::OpenFirst:
      for (std::size_t value = 0; value < threshold; ++value) {
        result.vertical.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value + 1, result.width)});
        ++result.vertical.arc_count;
      }
      result.horizontal.arc_count = checkedAdd(
          result.horizontal.arc_count, result.width, "horizontal arm arcs");
      break;

    case UnaryLabel::CloseFirst:
      for (std::size_t value = 1; value < result.width; ++value) {
        result.vertical.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value - 1, result.width)});
        ++result.vertical.arc_count;
      }
      for (std::size_t value = 0; value < result.width; ++value) {
        result.horizontal.closing_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value, result.width), 0});
        ++result.horizontal.arc_count;
      }
      break;

    case UnaryLabel::OpenSecond:
      result.vertical.arc_count = checkedAdd(result.vertical.arc_count,
                                             result.width, "vertical arm arcs");
      for (std::size_t value = 0; value < threshold; ++value) {
        result.horizontal.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value + 1, result.width)});
        ++result.horizontal.arc_count;
      }
      break;

    case UnaryLabel::CloseSecond:
      for (std::size_t value = 0; value < result.width; ++value) {
        result.vertical.closing_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value, result.width), 0});
        ++result.vertical.arc_count;
      }
      for (std::size_t value = 1; value < result.width; ++value) {
        result.horizontal.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value - 1, result.width)});
        ++result.horizontal.arc_count;
      }
      break;
    }
  }
  return result;
}

struct RootLabel {
  std::size_t component = 0;
  std::size_t residual_height = 0;

  bool operator==(const RootLabel &other) const {
    return component == other.component &&
           residual_height == other.residual_height;
  }
};

struct LabeledObject {
  RootLabel label;
  std::size_t object = 0;
};

template <typename Key>
void countingSort(std::vector<LabeledObject> &values,
                  std::vector<LabeledObject> &scratch, std::size_t base,
                  Key key) {
  std::vector<std::size_t> offsets(base, 0);
  for (const LabeledObject &value : values) {
    ++offsets.at(key(value));
  }
  std::size_t next = 0;
  for (std::size_t &offset : offsets) {
    const std::size_t count = offset;
    offset = next;
    next += count;
  }
  for (const LabeledObject &value : values) {
    scratch[offsets[key(value)]++] = value;
  }
  values.swap(scratch);
}

void mergeEqualVerticalLabels(std::vector<LabeledObject> &labels,
                              std::size_t base, DisjointSets &merged) {
  std::vector<LabeledObject> scratch(labels.size());
  countingSort(labels, scratch, base, [](const LabeledObject &value) {
    return value.label.residual_height;
  });
  countingSort(labels, scratch, base, [](const LabeledObject &value) {
    return value.label.component;
  });
  for (std::size_t i = 1; i < labels.size(); ++i) {
    if (labels[i - 1].label == labels[i].label) {
      merged.join(labels[i - 1].object, labels[i].object);
    }
  }
}

std::vector<std::size_t> shallowComponents(const UnaryGraph &graph,
                                           std::size_t threshold,
                                           AdaptiveStats &stats) {
  stats.threshold = threshold;
  if (graph.vertex_count == 0) {
    return {};
  }

  Arms arms = buildArms(graph, threshold);
  stats.vertical_control_states = arms.vertical.control_states;
  stats.vertical_arcs = arms.vertical.arc_count;
  stats.horizontal_control_states = arms.horizontal.control_states;
  stats.horizontal_arcs = arms.horizontal.arc_count;

  const interleaved_dyck::BidirectedDyckResult vertical =
      BidirectedDyckComponentSolver{}.solve(arms.vertical.control_states, 1,
                                            arms.vertical.epsilon_edges,
                                            arms.vertical.closing_edges);
  const interleaved_dyck::BidirectedDyckResult horizontal =
      BidirectedDyckComponentSolver{}.solve(arms.horizontal.control_states, 1,
                                            arms.horizontal.epsilon_edges,
                                            arms.horizontal.closing_edges);
  const std::vector<std::size_t> &vertical_components = vertical.component;
  const std::vector<std::size_t> &horizontal_components = horizontal.component;
  stats.vertical_dyck = vertical.stats;
  stats.horizontal_dyck = horizontal.stats;

  std::vector<std::size_t> parent(arms.vertical.control_states, kNone);
  for (const LabeledStateEdge &edge : arms.vertical.closing_edges) {
    const std::size_t source = vertical_components[edge.source];
    const std::size_t target = vertical_components[edge.target];
    if (parent[source] == kNone) {
      parent[source] = target;
    } else if (parent[source] != target) {
      throw std::logic_error(
          "vertical one-counter parent is not well-defined for component " +
          std::to_string(source) + ": " + std::to_string(parent[source]) +
          " versus " + std::to_string(target));
    }
  }

  const std::size_t boundary_count =
      checkedMultiply(graph.vertex_count, arms.width, "adaptive boundary");
  const std::size_t object_count =
      checkedAdd(graph.vertex_count, boundary_count, "adaptive merge objects");
  DisjointSets merged(object_count);

  std::vector<LabeledObject> vertical_labels;
  vertical_labels.reserve(object_count);

  for (std::size_t vertex = 0; vertex < graph.vertex_count; ++vertex) {
    const std::size_t state = controlState(vertex, 0, arms.width);
    vertical_labels.push_back({{vertical_components[state], 0}, vertex});
  }

  for (std::size_t vertex = 0; vertex < graph.vertex_count; ++vertex) {
    std::size_t component =
        vertical_components[controlState(vertex, threshold, arms.width)];
    std::size_t residual_height = 0;
    for (std::size_t height = 0; height < arms.width; ++height) {
      const std::size_t boundary =
          graph.vertex_count + vertex * arms.width + height;
      vertical_labels.push_back({{component, residual_height}, boundary});
      if (residual_height == 0 && parent[component] != kNone) {
        component = parent[component];
      } else {
        ++residual_height;
      }
    }
  }
  mergeEqualVerticalLabels(vertical_labels, arms.vertical.control_states,
                           merged);

  std::vector<std::size_t> horizontal_representatives(
      arms.horizontal.control_states, kNone);
  for (std::size_t vertex = 0; vertex < graph.vertex_count; ++vertex) {
    for (std::size_t value = 0; value < arms.width; ++value) {
      const std::size_t component =
          horizontal_components[controlState(vertex, value, arms.width)];
      const std::size_t boundary =
          graph.vertex_count + vertex * arms.width + value;
      std::size_t &representative = horizontal_representatives[component];
      if (representative == kNone) {
        representative = boundary;
      } else {
        merged.join(representative, boundary);
      }
    }
  }

  std::vector<std::size_t> result(graph.vertex_count);
  std::vector<std::size_t> identifiers(object_count, kNone);
  std::size_t identifier_count = 0;
  for (std::size_t vertex = 0; vertex < graph.vertex_count; ++vertex) {
    const std::size_t root = merged.find(vertex);
    if (identifiers[root] == kNone) {
      identifiers[root] = identifier_count++;
    }
    result[vertex] = identifiers[root];
  }
  return result;
}

struct PartitionData {
  std::unordered_map<Vertex, std::size_t> components;
  AdaptiveStats stats;
};

PartitionData
computePartition(const UnaryProjection &canonical, const UnaryGraph &processed,
                 const std::vector<std::size_t> &original_to_processed,
                 std::size_t threshold, bool was_sparsified) {
  PartitionData result;
  result.stats.input_vertices = canonical.graph.vertex_count;
  result.stats.input_arcs = canonical.original_arc_count;
  result.stats.quotient_vertices = processed.vertex_count;
  result.stats.quotient_arcs = processed.edges.size();
  result.stats.added_reverse_arcs = canonical.added_reverse_arcs;
  result.stats.input_was_bidirected = canonical.added_reverse_arcs == 0;
  result.stats.overapproximates_original = canonical.added_reverse_arcs != 0;
  result.stats.sparsified = was_sparsified;

  const std::vector<std::size_t> processed_components =
      shallowComponents(processed, threshold, result.stats);
  std::vector<std::size_t> identifiers(processed.vertex_count, kNone);
  std::size_t identifier_count = 0;
  result.components.reserve(canonical.vertices.size());
  for (std::size_t vertex = 0; vertex < canonical.vertices.size(); ++vertex) {
    const std::size_t component =
        processed_components[original_to_processed[vertex]];
    if (identifiers[component] == kNone) {
      identifiers[component] = identifier_count++;
    }
    result.components.emplace(canonical.vertices[vertex],
                              identifiers[component]);
  }
  return result;
}

} // namespace

std::size_t AdaptiveResult::component(Vertex vertex) const {
  const auto found = components_.find(vertex);
  if (found == components_.end()) {
    throw std::out_of_range("unknown adaptive interleaved-Dyck vertex");
  }
  return found->second;
}

bool AdaptiveResult::connected(Vertex first, Vertex second) const {
  return component(first) == component(second);
}

AdaptiveResult AdaptiveSolver::solve(const Graph &graph,
                                     const AdaptiveOptions &options) const {
  const UnaryProjection canonical = projectToUnary(graph, options.input_policy);
  PartitionData partition;
  if (options.sparsify) {
    const UnaryQuotient quotient = sparsifyUnaryGraph(canonical.graph);
    const std::size_t threshold =
        checkedMultiply(6, quotient.graph.vertex_count, "adaptive threshold");
    partition =
        computePartition(canonical, quotient.graph,
                         quotient.original_to_quotient, threshold, true);
    partition.stats.quotient_dyck = quotient.dyck;
  } else {
    std::vector<std::size_t> identity(canonical.graph.vertex_count);
    for (std::size_t vertex = 0; vertex < identity.size(); ++vertex) {
      identity[vertex] = vertex;
    }
    const std::size_t threshold =
        checkedMultiply(6, canonical.graph.vertex_count, "adaptive threshold");
    partition = computePartition(canonical, canonical.graph, identity,
                                 threshold, false);
  }

  AdaptiveResult result;
  result.components_ = std::move(partition.components);
  result.stats_ = partition.stats;
  return result;
}

AdaptiveResult
AdaptiveSolver::solveShallow(const Graph &graph, std::size_t threshold,
                             const AdaptiveOptions &options) const {
  const UnaryProjection canonical = projectToUnary(graph, options.input_policy);
  std::vector<std::size_t> identity(canonical.graph.vertex_count);
  for (std::size_t vertex = 0; vertex < identity.size(); ++vertex) {
    identity[vertex] = vertex;
  }
  PartitionData partition =
      computePartition(canonical, canonical.graph, identity, threshold, false);
  AdaptiveResult result;
  result.components_ = std::move(partition.components);
  result.stats_ = partition.stats;
  return result;
}

} // namespace lotus::cfl::interleaved_dyck::unary
