#include "CFL/Classical/Solvers/Preprocessing/GraphSimplification.h"

#include "Utils/ADT/TarjanScc.h"

#include <algorithm>
#include <deque>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {
namespace {

class DisjointSet {
public:
  explicit DisjointSet(std::size_t size) : parent_(size), rank_(size) {
    std::iota(parent_.begin(), parent_.end(), 0);
  }

  std::size_t find(std::size_t node) {
    std::size_t root = node;
    while (parent_[root] != root) {
      root = parent_[root];
    }
    while (parent_[node] != node) {
      const std::size_t next = parent_[node];
      parent_[node] = root;
      node = next;
    }
    return root;
  }

  bool unite(std::size_t first, std::size_t second) {
    first = find(first);
    second = find(second);
    if (first == second) {
      return false;
    }
    if (rank_[first] < rank_[second] ||
        (rank_[first] == rank_[second] && second < first)) {
      std::swap(first, second);
    }
    parent_[second] = first;
    if (rank_[first] == rank_[second]) {
      ++rank_[first];
    }
    return true;
  }

  bool mergeInto(std::size_t node, std::size_t representative) {
    node = find(node);
    representative = find(representative);
    if (node == representative) {
      return false;
    }
    parent_[node] = representative;
    return true;
  }

private:
  std::vector<std::size_t> parent_;
  std::vector<unsigned> rank_;
};

bool isDirectLabel(const std::string &label, GraphSimplificationFlavor flavor) {
  if (flavor == GraphSimplificationFlavor::Alias) {
    return label == "a" || label == "copy" || label == "vgep";
  }
  return label == "a" || label == "direct" || label == "indirect" ||
         label == "thread";
}

bool isDereferenceLabel(const std::string &label) {
  return label == "d" || label == "addrbar";
}

bool isClientInputLabel(const std::string &label,
                        GraphSimplificationFlavor flavor) {
  if (flavor == GraphSimplificationFlavor::ValueFlow) {
    return isDirectLabel(label, flavor) || label.rfind("call_", 0) == 0 ||
           label.rfind("ret_", 0) == 0;
  }
  return isDirectLabel(label, flavor) || label == "d" || label == "addrbar" ||
         label == "f" || label == "gep" || label.rfind("f_", 0) == 0 ||
         label.rfind("gep_", 0) == 0;
}

std::pair<std::string, std::uint32_t>
splitAttributedLabel(const std::string &label) {
  const auto separator = label.find_last_of('_');
  if (separator == std::string::npos || separator + 1 == label.size()) {
    return {label, 0};
  }
  const std::string suffix = label.substr(separator + 1);
  if (suffix.find_first_not_of("0123456789") != std::string::npos) {
    return {label, 0};
  }
  return {label.substr(0, separator),
          static_cast<std::uint32_t>(std::stoul(suffix))};
}

bool isReverseLabel(const std::string &label,
                    GraphSimplificationFlavor flavor) {
  const auto [base, unused] = splitAttributedLabel(label);
  (void)unused;
  if (flavor == GraphSimplificationFlavor::Alias) {
    if (base == "addr") {
      return true;
    }
    if (base == "addrbar") {
      return false;
    }
  }
  return base.size() >= 3 && base.compare(base.size() - 3, 3, "bar") == 0;
}

LabeledGraph pruneInterDyck(const LabeledGraph &graph,
                            GraphSimplificationFlavor flavor,
                            GraphSimplificationStatistics &statistics) {
  struct SubEdge {
    std::size_t source = 0;
    std::size_t target = 0;
    std::string key;
  };

  DisjointSet sets(graph.vertexCount());
  std::vector<SubEdge> sub_edges;
  for (const LabeledEdge &edge : graph.edges()) {
    if (!isClientInputLabel(edge.label, flavor)) {
      continue;
    }
    if (isDirectLabel(edge.label, flavor)) {
      sets.mergeInto(edge.target, edge.source);
      continue;
    }
    const auto [base, attribute] = splitAttributedLabel(edge.label);
    if (flavor == GraphSimplificationFlavor::Alias) {
      if (base == "d" || base == "addrbar" || base == "f" || base == "gep") {
        sub_edges.push_back(
            {edge.source, edge.target, base + '_' + std::to_string(attribute)});
      }
      continue;
    }
    if (base == "call") {
      sub_edges.push_back(
          {edge.target, edge.source, std::to_string(attribute)});
    } else if (base == "ret") {
      sub_edges.push_back(
          {edge.source, edge.target, std::to_string(attribute)});
    }
  }

  std::set<std::pair<std::size_t, std::string>> anchors;
  std::deque<std::size_t> worklist;
  std::set<std::size_t> queued;
  std::unordered_map<std::size_t, std::set<std::size_t>> pending_merges;
  auto schedule = [&](std::size_t node) {
    node = sets.find(node);
    std::map<std::string, std::set<std::size_t>> targets;
    for (const SubEdge &edge : sub_edges) {
      const std::size_t source = sets.find(edge.source);
      const std::size_t target = sets.find(edge.target);
      if (source == node && source != target) {
        targets[edge.key].insert(target);
      }
    }
    for (const auto &[key, destinations] : targets) {
      if (destinations.size() < 2) {
        continue;
      }
      const std::size_t representative = *destinations.begin();
      pending_merges[representative] = destinations;
      anchors.insert({node, key});
      if (queued.insert(representative).second) {
        worklist.push_back(representative);
      }
    }
  };

  for (std::size_t node = 0; node < graph.vertexCount(); ++node) {
    if (sets.find(node) == node) {
      schedule(node);
    }
  }
  while (!worklist.empty()) {
    const std::size_t selected = worklist.front();
    worklist.pop_front();
    queued.erase(selected);
    const auto pending = pending_merges.find(selected);
    if (pending == pending_merges.end()) {
      continue;
    }
    const std::set<std::size_t> destinations = pending->second;
    for (std::size_t destination : destinations) {
      sets.mergeInto(destination, selected);
    }
    schedule(sets.find(selected));
  }

  std::set<std::pair<std::size_t, std::string>> normalized_anchors;
  for (const auto &[node, label] : anchors) {
    normalized_anchors.insert({sets.find(node), label});
  }

  using EdgeKey = std::tuple<std::size_t, std::size_t, std::string>;
  std::set<EdgeKey> kept_physical;
  for (const LabeledEdge &edge : graph.edges()) {
    if (!isClientInputLabel(edge.label, flavor)) {
      continue;
    }
    bool keep = isDirectLabel(edge.label, flavor);
    const auto [base, attribute] = splitAttributedLabel(edge.label);
    if (!keep && flavor == GraphSimplificationFlavor::Alias) {
      const std::string key = base + '_' + std::to_string(attribute);
      keep = normalized_anchors.count({sets.find(edge.source), key}) != 0;
    } else if (!keep && flavor == GraphSimplificationFlavor::ValueFlow) {
      const std::string key = std::to_string(attribute);
      if (base == "call") {
        keep = normalized_anchors.count({sets.find(edge.target), key}) != 0;
      } else if (base == "ret") {
        keep = normalized_anchors.count({sets.find(edge.source), key}) != 0;
      }
    }
    if (keep) {
      kept_physical.insert({edge.source, edge.target, edge.label});
    }
  }

  LabeledGraph pruned;
  for (const std::string &vertex : graph.vertices()) {
    pruned.addVertex(vertex);
  }
  for (std::size_t node = 0; node < graph.vertexCount(); ++node) {
    if (graph.isSource(node)) {
      pruned.markSource(node);
    }
  }
  for (const LabeledEdge &edge : graph.edges()) {
    bool keep =
        kept_physical.count({edge.source, edge.target, edge.label}) != 0;
    if (!keep && isReverseLabel(edge.label, flavor)) {
      keep =
          kept_physical.count({edge.target, edge.source,
                               LabeledGraph::complementLabel(edge.label)}) != 0;
    }
    if (!isClientInputLabel(edge.label, flavor) &&
        !isReverseLabel(edge.label, flavor)) {
      keep = true;
    }
    if (keep) {
      pruned.addEdge(edge.source, edge.target, edge.label);
    }
  }
  statistics.interdyck_edges_pruned += graph.edgeCount() - pruned.edgeCount();
  return pruned;
}

std::size_t countRoots(DisjointSet &sets, std::size_t nodes) {
  std::set<std::size_t> roots;
  for (std::size_t node = 0; node < nodes; ++node) {
    roots.insert(sets.find(node));
  }
  return roots.size();
}

void eliminateSccs(const LabeledGraph &graph, GraphSimplificationFlavor flavor,
                   DisjointSet &sets,
                   GraphSimplificationStatistics &statistics) {
  const std::size_t nodes = graph.vertexCount();
  std::vector<std::vector<std::size_t>> successors(nodes);
  for (const LabeledEdge &edge : graph.edges()) {
    if (isDirectLabel(edge.label, flavor)) {
      successors[edge.source].push_back(edge.target);
    }
  }
  auto successor_range = [&](std::size_t node) -> const auto & {
    return successors[node];
  };
  std::vector<std::size_t> scc_index;
  std::vector<std::size_t> reverse_topological_order;
  statistics.sccs = FindStronglyConnectedComponents(
      nodes, successor_range, scc_index, reverse_topological_order);

  std::unordered_map<std::size_t, std::size_t> representative;
  for (std::size_t node = 0; node < nodes; ++node) {
    const auto [it, inserted] = representative.emplace(scc_index[node], node);
    if (!inserted && sets.unite(it->second, node)) {
      ++statistics.scc_nodes_merged;
    }
  }
}

void foldGraph(const LabeledGraph &graph, GraphSimplificationFlavor flavor,
               DisjointSet &sets, GraphSimplificationStatistics &statistics) {
  using NormalizedEdge = std::tuple<std::size_t, std::size_t, std::string>;
  auto normalizedEdges = [&]() {
    std::set<NormalizedEdge> result;
    for (const LabeledEdge &edge : graph.edges()) {
      if (!isClientInputLabel(edge.label, flavor)) {
        continue;
      }
      const std::size_t source = sets.find(edge.source);
      const std::size_t target = sets.find(edge.target);
      if (source == target && isDirectLabel(edge.label, flavor)) {
        continue;
      }
      result.emplace(source, target, edge.label);
    }
    return result;
  };

  // POCR detects direct-edge foldable pairs once on the post-SCC graph.
  const std::set<NormalizedEdge> initial_edges = normalizedEdges();
  std::unordered_map<std::size_t, std::size_t> incoming_edges;
  std::unordered_map<std::size_t, std::size_t> incoming_direct_edges;
  std::unordered_map<std::size_t, bool> source_representative;
  for (std::size_t node = 0; node < graph.vertexCount(); ++node) {
    if (graph.isSource(node)) {
      source_representative[sets.find(node)] = true;
    }
  }
  for (const auto &[source, target, label] : initial_edges) {
    (void)source;
    ++incoming_edges[target];
    if (isDirectLabel(label, flavor)) {
      ++incoming_direct_edges[target];
    }
  }

  std::vector<std::pair<std::size_t, std::size_t>> foldable_pairs;
  for (const auto &[source, target, label] : initial_edges) {
    if (!isDirectLabel(label, flavor)) {
      continue;
    }
    const bool foldable =
        flavor == GraphSimplificationFlavor::Alias
            ? incoming_direct_edges[target] <= 1 &&
                  incoming_edges[target] == incoming_direct_edges[target]
            : incoming_edges[target] <= 1 && !source_representative[target];
    if (foldable) {
      foldable_pairs.emplace_back(source, target);
    }
  }
  while (!foldable_pairs.empty()) {
    const auto [source, target] = foldable_pairs.back();
    foldable_pairs.pop_back();
    if (sets.mergeInto(target, source)) {
      ++statistics.folded_nodes;
    }
  }

  if (flavor != GraphSimplificationFlavor::Alias) {
    return;
  }

  // PEGFold::mergeDeref is a separate dynamic phase after direct folding.
  std::deque<std::size_t> check_nodes;
  for (std::size_t node = 0; node < graph.vertexCount(); ++node) {
    if (sets.find(node) == node) {
      check_nodes.push_back(node);
    }
  }
  while (!check_nodes.empty()) {
    const std::size_t source = sets.find(check_nodes.front());
    check_nodes.pop_front();
    std::set<std::size_t> targets;
    for (const auto &[edge_source, edge_target, label] : normalizedEdges()) {
      if (edge_source == source && isDereferenceLabel(label)) {
        targets.insert(edge_target);
      }
    }
    if (targets.size() <= 1) {
      continue;
    }
    const std::size_t representative = *targets.begin();
    for (std::size_t target : targets) {
      if (sets.mergeInto(target, representative)) {
        ++statistics.common_dereference_nodes_merged;
      }
    }
    check_nodes.push_back(sets.find(representative));
  }
}

} // namespace

GraphSimplificationResult
simplifyGraph(const LabeledGraph &graph,
              const GraphSimplificationOptions &options) {
  GraphSimplificationResult result;
  result.statistics.original_nodes = graph.vertexCount();
  result.statistics.original_edges = graph.edgeCount();
  DisjointSet sets(graph.vertexCount());

  if (options.eliminate_sccs) {
    eliminateSccs(graph, options.flavor, sets, result.statistics);
  }
  if (options.fold_graph) {
    foldGraph(graph, options.flavor, sets, result.statistics);
  }

  std::unordered_map<std::size_t, std::size_t> reduced_ids;
  result.representative.resize(graph.vertexCount());
  for (std::size_t node = 0; node < graph.vertexCount(); ++node) {
    const std::size_t root = sets.find(node);
    auto [it, inserted] = reduced_ids.emplace(root, result.graph.vertexCount());
    if (inserted) {
      it->second = result.graph.addVertex(graph.vertexName(root));
      result.members.emplace_back();
    }
    result.representative[node] = it->second;
    result.members[it->second].push_back(node);
    if (graph.isSource(node)) {
      result.graph.markSource(it->second);
    }
  }

  for (const LabeledEdge &edge : graph.edges()) {
    const std::size_t source = result.representative[edge.source];
    const std::size_t target = result.representative[edge.target];
    if (source == target && isDirectLabel(edge.label, options.flavor)) {
      continue;
    }
    result.graph.addEdge(source, target, edge.label);
  }

  result.statistics.reduced_nodes = result.graph.vertexCount();
  result.statistics.reduced_edges = result.graph.edgeCount();
  const std::size_t expected_roots = countRoots(sets, graph.vertexCount());
  if (expected_roots != result.graph.vertexCount()) {
    throw std::logic_error("Graph simplification representative mismatch");
  }
  if (options.prune_interdyck) {
    result.graph =
        pruneInterDyck(result.graph, options.flavor, result.statistics);
    result.statistics.reduced_edges = result.graph.edgeCount();
  }
  return result;
}

} // namespace lotus::cfl::classical
