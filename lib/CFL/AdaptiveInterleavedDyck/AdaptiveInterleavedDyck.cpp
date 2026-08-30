#include "CFL/AdaptiveInterleavedDyck/AdaptiveInterleavedDyck.h"

#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::adaptive_interleaved_dyck {
namespace {

using interleaved_dyck::Edge;
using interleaved_dyck::Label;
using interleaved_dyck::LabelKind;

constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();

template <typename T> void hashCombine(std::size_t &seed, const T &value) {
  seed ^= std::hash<T>{}(value) + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}

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

enum class CounterLabel : unsigned char {
  Epsilon,
  OpenFirst,
  CloseFirst,
  OpenSecond,
  CloseSecond,
};

CounterLabel complement(CounterLabel label) {
  switch (label) {
  case CounterLabel::Epsilon:
    return CounterLabel::Epsilon;
  case CounterLabel::OpenFirst:
    return CounterLabel::CloseFirst;
  case CounterLabel::CloseFirst:
    return CounterLabel::OpenFirst;
  case CounterLabel::OpenSecond:
    return CounterLabel::CloseSecond;
  case CounterLabel::CloseSecond:
    return CounterLabel::OpenSecond;
  }
  throw std::logic_error("invalid unary interleaved-Dyck label");
}

CounterLabel projectCounterLabel(const Label &label) {
  switch (label.kind) {
  case LabelKind::Neutral:
    return CounterLabel::Epsilon;
  case LabelKind::OpenParenthesis:
    return CounterLabel::OpenFirst;
  case LabelKind::CloseParenthesis:
    return CounterLabel::CloseFirst;
  case LabelKind::OpenBracket:
    return CounterLabel::OpenSecond;
  case LabelKind::CloseBracket:
    return CounterLabel::CloseSecond;
  }
  throw std::logic_error("invalid interleaved-Dyck label kind");
}

struct DenseEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  CounterLabel label = CounterLabel::Epsilon;

  bool operator==(const DenseEdge &other) const {
    return source == other.source && target == other.target &&
           label == other.label;
  }
};

struct DenseEdgeHash {
  std::size_t operator()(const DenseEdge &edge) const {
    std::size_t seed = std::hash<std::size_t>{}(edge.source);
    hashCombine(seed, edge.target);
    hashCombine(seed, static_cast<unsigned char>(edge.label));
    return seed;
  }
};

struct DenseGraph {
  std::size_t vertex_count = 0;
  std::vector<DenseEdge> edges;
};

struct CanonicalGraph {
  DenseGraph graph;
  std::vector<Vertex> vertices;
  std::size_t original_arc_count = 0;
  std::size_t added_reverse_arcs = 0;
};

CanonicalGraph canonicalize(const Graph &input,
                            BidirectedInputPolicy input_policy) {
  CanonicalGraph result;
  result.vertices = input.vertices();
  result.graph.vertex_count = result.vertices.size();

  std::unordered_map<Vertex, std::size_t> indices;
  indices.reserve(result.vertices.size());
  for (std::size_t i = 0; i < result.vertices.size(); ++i) {
    indices.emplace(result.vertices[i], i);
  }

  std::unordered_set<DenseEdge, DenseEdgeHash> unique;
  unique.reserve(input.edges().size());
  for (const Edge &edge : input.edges()) {
    unique.insert({indices.at(edge.source), indices.at(edge.target),
                   projectCounterLabel(edge.label)});
  }
  result.original_arc_count = unique.size();

  const std::vector<DenseEdge> original_edges(unique.begin(), unique.end());
  for (const DenseEdge &edge : original_edges) {
    const DenseEdge reverse{edge.target, edge.source, complement(edge.label)};
    if (unique.count(reverse) == 0U) {
      if (input_policy == BidirectedInputPolicy::RequireBidirected) {
        throw std::invalid_argument(
            "adaptive interleaved-Dyck reachability requires a bidirected "
            "graph; missing reverse arc for " +
            std::to_string(result.vertices[edge.source]) + " -> " +
            std::to_string(result.vertices[edge.target]));
      }
      unique.insert(reverse);
      ++result.added_reverse_arcs;
    }
  }
  result.graph.edges.assign(unique.begin(), unique.end());
  return result;
}

struct IndexPair {
  std::size_t source = 0;
  std::size_t target = 0;
};

class DisjointSets {
public:
  explicit DisjointSets(std::size_t size) : parent_(size), size_(size, 1) {
    for (std::size_t i = 0; i < size; ++i) {
      parent_[i] = i;
    }
  }

  DisjointSets(std::size_t size, const std::vector<IndexPair> &edges)
      : parent_(size, kNone), size_(size, 0) {
    std::vector<std::size_t> heads(size, kNone);
    std::vector<std::size_t> targets;
    std::vector<std::size_t> next;
    const std::size_t adjacency_size =
        checkedMultiply(2, edges.size(), "epsilon adjacency");
    targets.reserve(adjacency_size);
    next.reserve(adjacency_size);
    const auto add_edge = [&](std::size_t source, std::size_t target) {
      const std::size_t arc = targets.size();
      targets.push_back(target);
      next.push_back(heads.at(source));
      heads[source] = arc;
    };
    for (const IndexPair &edge : edges) {
      add_edge(edge.source, edge.target);
      add_edge(edge.target, edge.source);
    }

    std::vector<std::size_t> worklist;
    for (std::size_t root = 0; root < size; ++root) {
      if (parent_[root] != kNone) {
        continue;
      }
      parent_[root] = root;
      worklist.push_back(root);
      while (!worklist.empty()) {
        const std::size_t source = worklist.back();
        worklist.pop_back();
        ++size_[root];
        for (std::size_t arc = heads[source]; arc != kNone; arc = next[arc]) {
          const std::size_t target = targets[arc];
          if (parent_[target] == kNone) {
            parent_[target] = root;
            worklist.push_back(target);
          }
        }
      }
    }
  }

  std::size_t find(std::size_t element) {
    std::size_t &parent = parent_.at(element);
    if (parent != element) {
      parent = find(parent);
    }
    return parent;
  }

  std::pair<std::size_t, std::size_t> joinRoots(std::size_t first,
                                                std::size_t second) {
    first = find(first);
    second = find(second);
    if (first == second) {
      return {first, kNone};
    }
    if (size_[first] < size_[second]) {
      std::swap(first, second);
    }
    parent_[second] = first;
    size_[first] += size_[second];
    return {first, second};
  }

  void join(std::size_t first, std::size_t second) { joinRoots(first, second); }

private:
  std::vector<std::size_t> parent_;
  std::vector<std::size_t> size_;
};

struct ClosingEdge {
  std::size_t source = 0;
  std::size_t target = 0;
  std::size_t label = 0;
};

struct LinkedList {
  std::size_t head = kNone;
  std::size_t tail = kNone;
  std::size_t size = 0;
};

class EdgePool {
public:
  void append(LinkedList &list, std::size_t target) {
    const std::size_t node = targets_.size();
    targets_.push_back(target);
    next_.push_back(kNone);
    appendNode(list, node);
  }

  void splice(LinkedList &destination, LinkedList &source) {
    if (source.size == 0) {
      return;
    }
    if (destination.size == 0) {
      destination = source;
    } else {
      next_[destination.tail] = source.head;
      destination.tail = source.tail;
      destination.size += source.size;
    }
    source = {};
  }

  LinkedList detach(LinkedList &list) {
    const LinkedList result = list;
    list = {};
    return result;
  }

  void appendReusedSingleton(LinkedList &destination, const LinkedList &old,
                             std::size_t target) {
    if (old.head == kNone) {
      append(destination, target);
      return;
    }
    targets_[old.head] = target;
    next_[old.head] = kNone;
    LinkedList singleton{old.head, old.head, 1};
    splice(destination, singleton);
  }

  std::size_t target(std::size_t node) const { return targets_.at(node); }
  std::size_t next(std::size_t node) const { return next_.at(node); }

private:
  void appendNode(LinkedList &list, std::size_t node) {
    if (list.size == 0) {
      list = {node, node, 1};
      return;
    }
    next_[list.tail] = node;
    list.tail = node;
    ++list.size;
  }

  std::vector<std::size_t> targets_;
  std::vector<std::size_t> next_;
};

/// Chatterjee-Choudhary-Pavlogiannis bidirected-Dyck component closure.
/// Only closing arcs are stored; bidirectedness supplies their opening
/// reverses. Epsilon endpoints are contracted before the worklist starts.
class ClosingComponentSolver {
public:
  ClosingComponentSolver(std::size_t vertex_count, std::size_t label_count,
                         const std::vector<IndexPair> &epsilon_edges,
                         const std::vector<ClosingEdge> &closing_edges)
      : sets_(vertex_count, epsilon_edges), label_count_(label_count),
        lists_(checkedMultiply(vertex_count, label_count,
                               "bidirected-Dyck edge table")),
        queued_(lists_.size(), false), seen_(vertex_count, 0) {
    for (const ClosingEdge &edge : closing_edges) {
      if (edge.label >= label_count_) {
        throw std::logic_error("closing label index is out of range");
      }
      pool_.append(list(sets_.find(edge.source), edge.label), edge.target);
    }
  }

  std::vector<std::size_t> solve() {
    const std::size_t vertex_count = seen_.size();
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
      if (sets_.find(vertex) != vertex) {
        continue;
      }
      for (std::size_t label = 0; label < label_count_; ++label) {
        enqueue(vertex, label);
      }
    }

    while (!worklist_.empty()) {
      const auto [queued_vertex, label] = worklist_.front();
      worklist_.pop_front();
      queued_[index(queued_vertex, label)] = false;

      const std::size_t source = sets_.find(queued_vertex);
      if (source != queued_vertex) {
        enqueue(source, label);
        continue;
      }

      LinkedList old = pool_.detach(list(source, label));
      std::vector<std::size_t> targets;
      nextGeneration();
      for (std::size_t node = old.head; node != kNone;
           node = pool_.next(node)) {
        const std::size_t target = sets_.find(pool_.target(node));
        if (seen_[target] != generation_) {
          seen_[target] = generation_;
          targets.push_back(target);
        }
      }

      if (targets.empty()) {
        continue;
      }

      std::size_t target = targets.front();
      for (std::size_t i = 1; i < targets.size(); ++i) {
        target = joinWithLists(target, targets[i]);
      }
      target = sets_.find(target);

      const std::size_t current_source = sets_.find(source);
      // If source is one of the merged targets, this becomes a self edge at
      // the new representative. Keeping that edge is essential: together
      // with another closing edge of the merged component it can trigger the
      // next Dyck-component union (Algorithm 1, lines 20--23).
      pool_.appendReusedSingleton(list(current_source, label), old, target);
      enqueue(current_source, label);
    }

    std::vector<std::size_t> components(vertex_count);
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
      components[vertex] = sets_.find(vertex);
    }
    return components;
  }

private:
  std::size_t index(std::size_t vertex, std::size_t label) const {
    return vertex * label_count_ + label;
  }

  LinkedList &list(std::size_t vertex, std::size_t label) {
    return lists_[index(vertex, label)];
  }

  void enqueue(std::size_t vertex, std::size_t label) {
    vertex = sets_.find(vertex);
    const std::size_t slot = index(vertex, label);
    if (lists_[slot].size >= 2 && !queued_[slot]) {
      queued_[slot] = true;
      worklist_.emplace_back(vertex, label);
    }
  }

  std::size_t joinWithLists(std::size_t first, std::size_t second) {
    const auto [root, removed] = sets_.joinRoots(first, second);
    if (removed == kNone) {
      return root;
    }
    for (std::size_t label = 0; label < label_count_; ++label) {
      pool_.splice(list(root, label), list(removed, label));
      enqueue(root, label);
    }
    return root;
  }

  void nextGeneration() {
    ++generation_;
    if (generation_ == 0) {
      std::fill(seen_.begin(), seen_.end(), 0);
      generation_ = 1;
    }
  }

  DisjointSets sets_;
  std::size_t label_count_ = 0;
  std::vector<LinkedList> lists_;
  std::vector<unsigned char> queued_;
  std::deque<std::pair<std::size_t, std::size_t>> worklist_;
  EdgePool pool_;
  std::vector<std::size_t> seen_;
  std::size_t generation_ = 0;
};

struct SparsifiedGraph {
  DenseGraph graph;
  std::vector<std::size_t> original_to_quotient;
};

SparsifiedGraph sparsify(const DenseGraph &graph) {
  std::vector<IndexPair> epsilon_edges;
  std::vector<ClosingEdge> closing_edges;
  epsilon_edges.reserve(graph.edges.size());
  closing_edges.reserve(graph.edges.size() / 2);
  for (const DenseEdge &edge : graph.edges) {
    switch (edge.label) {
    case CounterLabel::Epsilon:
      epsilon_edges.push_back({edge.source, edge.target});
      break;
    case CounterLabel::CloseFirst:
      closing_edges.push_back({edge.source, edge.target, 0});
      break;
    case CounterLabel::CloseSecond:
      closing_edges.push_back({edge.source, edge.target, 1});
      break;
    case CounterLabel::OpenFirst:
    case CounterLabel::OpenSecond:
      break;
    }
  }

  const std::vector<std::size_t> components =
      ClosingComponentSolver(graph.vertex_count, 2, epsilon_edges,
                             closing_edges)
          .solve();

  SparsifiedGraph result;
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
  for (const DenseEdge &edge : graph.edges) {
    std::size_t label = 0;
    if (edge.label == CounterLabel::CloseSecond) {
      label = 1;
    } else if (edge.label != CounterLabel::CloseFirst) {
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
      const CounterLabel close =
          label == 0 ? CounterLabel::CloseFirst : CounterLabel::CloseSecond;
      result.graph.edges.push_back({source, target, close});
      result.graph.edges.push_back({target, source, complement(close)});
    }
  }
  return result;
}

struct OneCounterArm {
  std::size_t control_states = 0;
  std::size_t arc_count = 0;
  std::vector<IndexPair> epsilon_edges;
  std::vector<ClosingEdge> closing_edges;
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

Arms buildArms(const DenseGraph &graph, std::size_t threshold) {
  Arms result;
  result.width = checkedAdd(threshold, 1, "adaptive threshold");
  const std::size_t state_count =
      checkedMultiply(graph.vertex_count, result.width, "arm state space");
  result.vertical.control_states = state_count;
  result.horizontal.control_states = state_count;

  for (const DenseEdge &edge : graph.edges) {
    switch (edge.label) {
    case CounterLabel::Epsilon:
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

    case CounterLabel::OpenFirst:
      for (std::size_t value = 0; value < threshold; ++value) {
        result.vertical.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value + 1, result.width)});
        ++result.vertical.arc_count;
      }
      result.horizontal.arc_count = checkedAdd(
          result.horizontal.arc_count, result.width, "horizontal arm arcs");
      break;

    case CounterLabel::CloseFirst:
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

    case CounterLabel::OpenSecond:
      result.vertical.arc_count = checkedAdd(result.vertical.arc_count,
                                             result.width, "vertical arm arcs");
      for (std::size_t value = 0; value < threshold; ++value) {
        result.horizontal.epsilon_edges.push_back(
            {controlState(edge.source, value, result.width),
             controlState(edge.target, value + 1, result.width)});
        ++result.horizontal.arc_count;
      }
      break;

    case CounterLabel::CloseSecond:
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

std::vector<std::size_t> shallowComponents(const DenseGraph &graph,
                                           std::size_t threshold,
                                           AdaptiveInterleavedStats &stats) {
  stats.threshold = threshold;
  if (graph.vertex_count == 0) {
    return {};
  }

  Arms arms = buildArms(graph, threshold);
  stats.vertical_control_states = arms.vertical.control_states;
  stats.vertical_arcs = arms.vertical.arc_count;
  stats.horizontal_control_states = arms.horizontal.control_states;
  stats.horizontal_arcs = arms.horizontal.arc_count;

  const std::vector<std::size_t> vertical_components =
      ClosingComponentSolver(arms.vertical.control_states, 1,
                             arms.vertical.epsilon_edges,
                             arms.vertical.closing_edges)
          .solve();
  const std::vector<std::size_t> horizontal_components =
      ClosingComponentSolver(arms.horizontal.control_states, 1,
                             arms.horizontal.epsilon_edges,
                             arms.horizontal.closing_edges)
          .solve();

  std::vector<std::size_t> parent(arms.vertical.control_states, kNone);
  for (const ClosingEdge &edge : arms.vertical.closing_edges) {
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
  AdaptiveInterleavedStats stats;
};

PartitionData
computePartition(const CanonicalGraph &canonical, const DenseGraph &processed,
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

std::size_t AdaptiveInterleavedResult::component(Vertex vertex) const {
  const auto found = components_.find(vertex);
  if (found == components_.end()) {
    throw std::out_of_range("unknown adaptive interleaved-Dyck vertex");
  }
  return found->second;
}

bool AdaptiveInterleavedResult::connected(Vertex first, Vertex second) const {
  return component(first) == component(second);
}

AdaptiveInterleavedResult AdaptiveInterleavedDyckSolver::solve(
    const Graph &graph, const AdaptiveInterleavedOptions &options) const {
  const CanonicalGraph canonical = canonicalize(graph, options.input_policy);
  PartitionData partition;
  if (options.sparsify) {
    const SparsifiedGraph quotient = sparsify(canonical.graph);
    const std::size_t threshold =
        checkedMultiply(6, quotient.graph.vertex_count, "adaptive threshold");
    partition =
        computePartition(canonical, quotient.graph,
                         quotient.original_to_quotient, threshold, true);
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

  AdaptiveInterleavedResult result;
  result.components_ = std::move(partition.components);
  result.stats_ = partition.stats;
  return result;
}

AdaptiveInterleavedResult AdaptiveInterleavedDyckSolver::solveShallow(
    const Graph &graph, std::size_t threshold,
    const AdaptiveInterleavedOptions &options) const {
  const CanonicalGraph canonical = canonicalize(graph, options.input_policy);
  std::vector<std::size_t> identity(canonical.graph.vertex_count);
  for (std::size_t vertex = 0; vertex < identity.size(); ++vertex) {
    identity[vertex] = vertex;
  }
  PartitionData partition =
      computePartition(canonical, canonical.graph, identity, threshold, false);
  AdaptiveInterleavedResult result;
  result.components_ = std::move(partition.components);
  result.stats_ = partition.stats;
  return result;
}

} // namespace lotus::cfl::adaptive_interleaved_dyck
