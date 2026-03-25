#include "IR/PDG/Analysis/SchedulingQuery.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_set>

namespace pdg {

namespace {

bool isEdgeAllowed(EdgeType et, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(et);
}

} // namespace

std::set<EdgeType> SchedulingQuery::defaultSchedulingEdgeTypes() {
  return {EdgeType::DATA_DEF_USE,  EdgeType::DATA_RAW,
          EdgeType::DATA_READ,     EdgeType::DATA_ALIAS,
          EdgeType::DATA_RET,      EdgeType::PARAMETER_IN,
          EdgeType::PARAMETER_OUT, EdgeType::PARAMETER_FIELD,
          EdgeType::VAL_DEP,       EdgeType::GLOBAL_DEP,
          EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
          EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
          EdgeType::CONTROLDEP_IND_BR};
}

IndependenceResult
SchedulingQuery::independent(Node &a, Node &b, const SchedulingPolicy &policy) {
  IndependenceResult result;
  if (&a == &b) {
    result.independent = false;
    result.witness_path_ab = {&a};
    return result;
  }

  const std::set<EdgeType> edge_types = policy.edge_types.empty()
                                            ? defaultSchedulingEdgeTypes()
                                            : policy.edge_types;

  bool a_to_b = findPath(a, b, edge_types, result.witness_path_ab,
                         result.witness_edge_types_ab);
  bool b_to_a = findPath(b, a, edge_types, result.witness_path_ba,
                         result.witness_edge_types_ba);
  result.independent = !a_to_b && !b_to_a;
  return result;
}

SchedulingQuery::NodeSet
SchedulingQuery::readySet(const NodeSet &region, const NodeSet &scheduled,
                          const SchedulingPolicy &policy) {
  NodeSet ready;
  const std::set<EdgeType> edge_types = policy.edge_types.empty()
                                            ? defaultSchedulingEdgeTypes()
                                            : policy.edge_types;

  for (Node *node : region) {
    if (node == nullptr || scheduled.count(node))
      continue;

    bool all_preds_satisfied = true;
    for (auto *edge : node->getInEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *pred = edge->getSrcNode();
      if (pred != nullptr && region.count(pred) && !scheduled.count(pred)) {
        all_preds_satisfied = false;
        break;
      }
    }
    if (all_preds_satisfied)
      ready.insert(node);
  }

  return ready;
}

std::unordered_map<Node *, std::vector<Node *>>
SchedulingQuery::buildAdjacency(const NodeSet &region,
                                const std::set<EdgeType> &edge_types) const {
  std::unordered_map<Node *, std::vector<Node *>> adj;
  for (Node *n : region) {
    if (n == nullptr)
      continue;
    auto &dsts = adj[n];
    for (auto *edge : n->getOutEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *dst = edge->getDstNode();
      if (dst != nullptr && region.count(dst))
        dsts.push_back(dst);
    }
  }
  return adj;
}

std::vector<SchedulingQuery::NodeSet>
SchedulingQuery::topologicalLevels(const NodeSet &region,
                                   const SchedulingPolicy &policy) {
  std::vector<NodeSet> levels;
  if (region.empty())
    return levels;

  const std::set<EdgeType> edge_types = policy.edge_types.empty()
                                            ? defaultSchedulingEdgeTypes()
                                            : policy.edge_types;
  auto sccs = stronglyConnectedComponents(region, policy);
  if (sccs.empty())
    return levels;

  std::unordered_map<Node *, size_t> node_to_scc;
  for (size_t i = 0; i < sccs.size(); ++i) {
    for (Node *node : sccs[i]) {
      if (node != nullptr)
        node_to_scc[node] = i;
    }
  }

  std::vector<std::set<size_t>> scc_adj(sccs.size());
  std::vector<size_t> indegree(sccs.size(), 0);
  for (Node *node : region) {
    if (node == nullptr)
      continue;
    const size_t src_scc = node_to_scc[node];
    for (auto *edge : node->getOutEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *dst = edge->getDstNode();
      if (dst == nullptr || !region.count(dst))
        continue;
      const size_t dst_scc = node_to_scc[dst];
      if (src_scc != dst_scc && scc_adj[src_scc].insert(dst_scc).second)
        ++indegree[dst_scc];
    }
  }

  std::queue<size_t> ready;
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (indegree[i] == 0)
      ready.push(i);
  }

  size_t emitted_sccs = 0;
  while (!ready.empty()) {
    const size_t layer_size = ready.size();
    NodeSet level;
    for (size_t i = 0; i < layer_size; ++i) {
      const size_t scc_idx = ready.front();
      ready.pop();
      ++emitted_sccs;
      level.insert(sccs[scc_idx].begin(), sccs[scc_idx].end());
      for (size_t succ : scc_adj[scc_idx]) {
        if (--indegree[succ] == 0)
          ready.push(succ);
      }
    }
    if (!level.empty())
      levels.push_back(std::move(level));
  }

  if (emitted_sccs != sccs.size()) {
    for (size_t i = 0; i < sccs.size(); ++i) {
      if (indegree[i] == 0)
        continue;
      levels.push_back(sccs[i]);
    }
  }

  return levels;
}

std::vector<SchedulingQuery::NodeSet>
SchedulingQuery::stronglyConnectedComponents(const NodeSet &region,
                                             const SchedulingPolicy &policy) {
  std::vector<NodeSet> sccs;
  if (region.empty())
    return sccs;

  const std::set<EdgeType> edge_types = policy.edge_types.empty()
                                            ? defaultSchedulingEdgeTypes()
                                            : policy.edge_types;
  auto adj = buildAdjacency(region, edge_types);

  std::unordered_map<Node *, int> index;
  std::unordered_map<Node *, int> lowlink;
  std::unordered_set<Node *> on_stack;
  std::vector<Node *> stack;
  int current_index = 0;

  std::function<void(Node *)> strongconnect = [&](Node *v) {
    index[v] = current_index;
    lowlink[v] = current_index;
    ++current_index;
    stack.push_back(v);
    on_stack.insert(v);

    for (Node *w : adj[v]) {
      if (!index.count(w)) {
        strongconnect(w);
        lowlink[v] = std::min(lowlink[v], lowlink[w]);
      } else if (on_stack.count(w)) {
        lowlink[v] = std::min(lowlink[v], index[w]);
      }
    }

    if (lowlink[v] == index[v]) {
      NodeSet comp;
      while (!stack.empty()) {
        Node *w = stack.back();
        stack.pop_back();
        on_stack.erase(w);
        comp.insert(w);
        if (w == v)
          break;
      }
      sccs.push_back(std::move(comp));
    }
  };

  for (Node *n : region) {
    if (n != nullptr && !index.count(n))
      strongconnect(n);
  }

  return sccs;
}

size_t SchedulingQuery::criticalPathLength(const NodeSet &region,
                                           const SchedulingPolicy &policy) {
  if (region.empty())
    return 0;

  const std::set<EdgeType> edge_types = policy.edge_types.empty()
                                            ? defaultSchedulingEdgeTypes()
                                            : policy.edge_types;
  auto sccs = stronglyConnectedComponents(region, policy);

  std::unordered_map<Node *, size_t> node_to_comp;
  for (size_t i = 0; i < sccs.size(); ++i) {
    for (Node *n : sccs[i])
      node_to_comp[n] = i;
  }

  std::vector<std::set<size_t>> comp_adj(sccs.size());
  std::vector<size_t> indegree(sccs.size(), 0);
  for (Node *n : region) {
    if (n == nullptr)
      continue;
    const size_t src_c = node_to_comp[n];
    for (auto *edge : n->getOutEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *dst = edge->getDstNode();
      if (dst == nullptr || !region.count(dst))
        continue;
      const size_t dst_c = node_to_comp[dst];
      if (src_c != dst_c && comp_adj[src_c].insert(dst_c).second)
        ++indegree[dst_c];
    }
  }

  std::queue<size_t> q;
  for (size_t i = 0; i < sccs.size(); ++i) {
    if (indegree[i] == 0)
      q.push(i);
  }

  std::vector<size_t> dist(sccs.size(), 0);
  for (size_t i = 0; i < sccs.size(); ++i)
    dist[i] = sccs[i].size();

  while (!q.empty()) {
    const size_t c = q.front();
    q.pop();
    for (size_t succ : comp_adj[c]) {
      dist[succ] = std::max(dist[succ], dist[c] + sccs[succ].size());
      if (--indegree[succ] == 0)
        q.push(succ);
    }
  }

  size_t best = 0;
  for (size_t d : dist)
    best = std::max(best, d);

  // Convert from node-count estimate to edge-count estimate.
  return best > 0 ? best - 1 : 0;
}

bool SchedulingQuery::findPath(Node &source, Node &target,
                               const std::set<EdgeType> &edge_types,
                               std::vector<Node *> &path,
                               std::vector<EdgeType> &path_edge_types) const {
  path.clear();
  path_edge_types.clear();

  if (&source == &target) {
    path.push_back(&source);
    return true;
  }

  std::queue<Node *> worklist;
  std::unordered_set<Node *> visited;
  std::unordered_map<Node *, Node *> pred;
  std::unordered_map<Node *, EdgeType> pred_edge;

  worklist.push(&source);
  visited.insert(&source);
  pred[&source] = nullptr;

  bool found = false;
  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();
    for (auto *edge : current->getOutEdgeSet()) {
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *next = edge->getDstNode();
      if (next == nullptr || visited.count(next))
        continue;
      visited.insert(next);
      pred[next] = current;
      pred_edge[next] = edge->getEdgeType();
      if (next == &target) {
        found = true;
        break;
      }
      worklist.push(next);
    }
    if (found)
      break;
  }

  if (!found)
    return false;

  for (Node *n = &target; n != nullptr; n = pred[n]) {
    path.push_back(n);
    if (pred[n] != nullptr)
      path_edge_types.push_back(pred_edge[n]);
  }
  std::reverse(path.begin(), path.end());
  std::reverse(path_edge_types.begin(), path_edge_types.end());
  return true;
}

} // namespace pdg
