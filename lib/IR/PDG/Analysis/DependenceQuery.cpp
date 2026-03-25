/**
 * @file DependenceQuery.cpp
 * @brief Implementation of pairwise dependence queries, transitive closure,
 *        and dependence distance computation over the PDG.
 *
 * References:
 * - Ferrante, Ottenstein & Warren, "The Program Dependence Graph and Its Use
 *   in Optimization", TOPLAS 1987.
 * - Horwitz, Reps & Binkley, "Interprocedural Slicing Using Dependence
 *   Graphs", TOPLAS 1990.
 */

#include "IR/PDG/Analysis/DependenceQuery.h"

#include "IR/PDG/Support/PDGUtils.h"

#include <algorithm>
#include <chrono>
#include <queue>

using namespace llvm;

namespace pdg {

// ============================================================================
// Helpers
// ============================================================================

namespace {

bool isEdgeAllowed(EdgeType et, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(et);
}

} // namespace

// ============================================================================
// PairwiseDependence
// ============================================================================

DependenceResult
PairwiseDependence::query(Node &source, Node &target,
                          const std::set<EdgeType> &edge_types) {
  DependenceResult result;

  if (&source == &target) {
    result.has_dependence = true;
    result.is_direct = false;
    result.is_transitive = false;
    result.distance = 0;
    result.witness_path = {&source};
    return result;
  }

  // BFS from source to target, recording predecessor map for witness path.
  std::unordered_map<Node *, Node *> pred;        // child -> parent
  std::unordered_map<Node *, EdgeType> pred_edge; // child -> edge from parent
  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist; // <node, depth>

  worklist.push({&source, 0});
  visited.insert(&source);
  pred[&source] = nullptr;

  bool found = false;

  while (!worklist.empty()) {
    auto pair = worklist.front();
    Node *current = pair.first;
    size_t depth = pair.second;
    worklist.pop();

    for (auto *edge : current->getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;

      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr || visited.count(neighbor))
        continue;

      visited.insert(neighbor);
      pred[neighbor] = current;
      pred_edge[neighbor] = edge->getEdgeType();

      if (neighbor == &target) {
        found = true;
        break;
      }

      worklist.push({neighbor, depth + 1});
    }

    if (found)
      break;
  }

  if (!found)
    return result;

  // Reconstruct witness path.
  result.has_dependence = true;
  std::vector<Node *> path;
  std::vector<EdgeType> edge_path;
  for (Node *n = &target; n != nullptr; n = pred[n]) {
    path.push_back(n);
    if (pred.count(n) && pred[n] != nullptr)
      edge_path.push_back(pred_edge[n]);
  }
  std::reverse(path.begin(), path.end());
  std::reverse(edge_path.begin(), edge_path.end());

  result.witness_path = std::move(path);
  result.witness_edge_types = std::move(edge_path);
  result.distance = result.witness_path.size() - 1;
  result.is_direct = (result.distance == 1);
  result.is_transitive = (result.distance > 1);

  return result;
}

PairwiseDependence::DirectDepMap
PairwiseDependence::directDependences(Node &node, bool forward,
                                      const std::set<EdgeType> &edge_types) {
  DirectDepMap result;
  auto &edges = forward ? node.getOutEdgeSet() : node.getInEdgeSet();

  for (auto *edge : edges) {
    if (edge == nullptr)
      continue;
    if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
      continue;

    Node *neighbor = forward ? edge->getDstNode() : edge->getSrcNode();
    if (neighbor != nullptr)
      result[neighbor].insert(edge->getEdgeType());
  }

  return result;
}

std::vector<std::vector<Node *>>
PairwiseDependence::allShortestPaths(Node &source, Node &target,
                                     const std::set<EdgeType> &edge_types,
                                     size_t max_paths) {
  std::vector<std::vector<Node *>> result;

  if (&source == &target) {
    result.push_back({&source});
    return result;
  }

  // BFS that records *all* predecessors at the shortest distance.
  std::unordered_map<Node *, size_t> dist;
  std::unordered_map<Node *, std::vector<Node *>> preds;
  std::queue<Node *> worklist;

  dist[&source] = 0;
  worklist.push(&source);

  size_t target_dist = SIZE_MAX;

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();

    size_t d = dist[current];
    if (d >= target_dist)
      continue;

    for (auto *edge : current->getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;

      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr)
        continue;

      size_t nd = d + 1;

      if (dist.find(neighbor) == dist.end()) {
        dist[neighbor] = nd;
        preds[neighbor].push_back(current);
        if (neighbor == &target)
          target_dist = nd;
        else
          worklist.push(neighbor);
      } else if (dist[neighbor] == nd) {
        preds[neighbor].push_back(current);
      }
    }
  }

  if (target_dist == SIZE_MAX)
    return result;

  // Backtrack from target to source to enumerate all shortest paths.
  std::vector<Node *> current_path = {&target};
  std::function<void()> enumerate = [&]() {
    Node *head = current_path.back();
    if (head == &source) {
      std::vector<Node *> path(current_path.rbegin(), current_path.rend());
      result.push_back(std::move(path));
      return;
    }
    if (max_paths > 0 && result.size() >= max_paths)
      return;
    for (Node *p : preds[head]) {
      current_path.push_back(p);
      enumerate();
      current_path.pop_back();
      if (max_paths > 0 && result.size() >= max_paths)
        return;
    }
  };
  enumerate();

  return result;
}

// ============================================================================
// TransitiveClosure
// ============================================================================

void TransitiveClosure::build(const std::set<EdgeType> &edge_types,
                              TransitiveClosureDiagnostics *diagnostics) {
  NodeSet all_nodes(_pdg.begin(), _pdg.end());
  build(all_nodes, edge_types, diagnostics);
}

void TransitiveClosure::build(const NodeSet &subgraph_nodes,
                              const std::set<EdgeType> &edge_types,
                              TransitiveClosureDiagnostics *diagnostics) {
  reset();
  auto t0 = std::chrono::steady_clock::now();

  // Initialize maps.
  for (Node *n : subgraph_nodes) {
    _forward[n]; // create empty entry
    _reverse[n];
  }

  // For every node, BFS forward collecting reachable nodes.
  for (Node *src : subgraph_nodes) {
    std::unordered_set<Node *> visited;
    std::queue<Node *> worklist;
    worklist.push(src);
    visited.insert(src);

    while (!worklist.empty()) {
      Node *current = worklist.front();
      worklist.pop();

      for (auto *edge : current->getOutEdgeSet()) {
        if (edge == nullptr)
          continue;
        if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
          continue;

        Node *neighbor = edge->getDstNode();
        if (neighbor == nullptr || visited.count(neighbor))
          continue;
        // Only track nodes inside the sub-graph.
        if (_forward.find(neighbor) == _forward.end())
          continue;

        visited.insert(neighbor);
        _forward[src].insert(neighbor);
        _reverse[neighbor].insert(src);
        worklist.push(neighbor);
      }
    }
  }

  _is_built = true;

  if (diagnostics) {
    auto t1 = std::chrono::steady_clock::now();
    diagnostics->num_nodes = subgraph_nodes.size();
    diagnostics->num_reachable_pairs = 0;
    for (auto &kv : _forward)
      diagnostics->num_reachable_pairs += kv.second.size();
    diagnostics->build_time_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
  }
}

bool TransitiveClosure::canReach(Node &source, Node &target) const {
  auto it = _forward.find(&source);
  if (it == _forward.end())
    return false;
  return it->second.count(&target);
}

TransitiveClosure::NodeSet
TransitiveClosure::getReachableSet(Node &source) const {
  NodeSet result;
  auto it = _forward.find(&source);
  if (it != _forward.end())
    result.insert(it->second.begin(), it->second.end());
  return result;
}

TransitiveClosure::NodeSet
TransitiveClosure::getPredecessorSet(Node &target) const {
  NodeSet result;
  auto it = _reverse.find(&target);
  if (it != _reverse.end())
    result.insert(it->second.begin(), it->second.end());
  return result;
}

void TransitiveClosure::reset() {
  _forward.clear();
  _reverse.clear();
  _is_built = false;
}

// ============================================================================
// DependenceDistance
// ============================================================================

template <typename GetEdgesFunc, typename GetNeighborFunc>
DependenceDistance::DistanceMap DependenceDistance::computeDistances(
    Node &start, const std::set<EdgeType> &edge_types, size_t max_depth,
    GetEdgesFunc get_edges, GetNeighborFunc get_neighbor) {
  DistanceMap distances;
  std::queue<std::pair<Node *, size_t>> worklist;
  distances[&start] = 0;
  worklist.push({&start, 0});

  while (!worklist.empty()) {
    auto pair = worklist.front();
    Node *current = pair.first;
    size_t depth = pair.second;
    worklist.pop();

    if (max_depth > 0 && depth >= max_depth)
      continue;

    for (auto *edge : get_edges(current)) {
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;

      Node *neighbor = get_neighbor(edge);
      if (neighbor == nullptr)
        continue;

      size_t nd = depth + 1;
      if (distances.find(neighbor) == distances.end()) {
        distances[neighbor] = nd;
        worklist.push({neighbor, nd});
      }
    }
  }

  return distances;
}

size_t DependenceDistance::distance(Node &source, Node &target,
                                    const std::set<EdgeType> &edge_types) {
  if (&source == &target)
    return 0;

  // Bidirectional BFS could be used here but plain BFS is simpler and
  // sufficient for moderate-sized PDGs.
  auto dists = forwardDistances(source, edge_types);
  auto it = dists.find(&target);
  return (it != dists.end()) ? it->second : SIZE_MAX;
}

DependenceDistance::DistanceMap DependenceDistance::forwardDistances(
    Node &source, const std::set<EdgeType> &edge_types, size_t max_depth) {
  return computeDistances(
      source, edge_types, max_depth,
      [](Node *n) -> Node::EdgeSet & { return n->getOutEdgeSet(); },
      [](Edge *e) { return e->getDstNode(); });
}

DependenceDistance::DistanceMap DependenceDistance::backwardDistances(
    Node &target, const std::set<EdgeType> &edge_types, size_t max_depth) {
  return computeDistances(
      target, edge_types, max_depth,
      [](Node *n) -> Node::EdgeSet & { return n->getInEdgeSet(); },
      [](Edge *e) { return e->getSrcNode(); });
}

size_t DependenceDistance::eccentricity(Node &node,
                                        const std::set<EdgeType> &edge_types) {
  auto dists = forwardDistances(node, edge_types);
  size_t max_dist = 0;
  for (auto &kv : dists)
    max_dist = std::max(max_dist, kv.second);
  return max_dist;
}

} // namespace pdg
