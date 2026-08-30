#include "IR/PDG/Analysis/DependenceQuery.h"

#include "IR/PDG/Analysis/Internal/QuerySupport.h"
#include "IR/PDG/Analysis/Query.h"

#include <algorithm>
#include <functional>
#include <queue>
#include <unordered_set>

namespace pdg {
using namespace llvm;
using namespace query_detail;

DependenceQuery::DependenceQuery(ProgramGraph &pdg) : pdg_(pdg) {}

PDGQueryResult DependenceQuery::reachability(const PDGCriteria &sources,
                                             const PDGQueryOptions &options,
                                             const Module *module) const {
  SliceQuery slice(pdg_);
  return slice.forward(sources, options, module);
}

PDGQueryResult DependenceQuery::shortestPath(const PDGCriteria &sources,
                                             const PDGCriteria &targets,
                                             const PDGQueryOptions &options,
                                             const Module *module) const {
  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult source_nodes = resolver.resolve(sources, options, module);
  PDGQueryResult target_nodes = resolver.resolve(targets, options, module);
  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);

  std::queue<Node *> worklist;
  std::unordered_map<Node *, size_t> distances;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>> preds;

  for (NodeSet::const_iterator it = source_nodes.nodes.begin();
       it != source_nodes.nodes.end(); ++it) {
    worklist.push(*it);
    distances[*it] = 0;
  }

  size_t best_distance = static_cast<size_t>(-1);
  Node *best_target = nullptr;

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();
    const size_t distance_to_current = distances[current];
    if (distance_to_current >= best_distance)
      continue;

    for (Node::EdgeSet::const_iterator it = current->getOutEdgeSet().begin();
         it != current->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr)
        continue;
      if (!scoped_nodes.empty() && scoped_nodes.count(neighbor) == 0)
        continue;

      const size_t next_distance = distance_to_current + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          distances.find(neighbor);
      if (dist_it == distances.end() || next_distance < dist_it->second) {
        distances[neighbor] = next_distance;
        preds[neighbor].clear();
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
        worklist.push(neighbor);
      } else if (next_distance == dist_it->second) {
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
      }

      if (target_nodes.nodes.count(neighbor) != 0 &&
          next_distance < best_distance) {
        best_distance = next_distance;
        best_target = neighbor;
      }
    }
  }

  PDGQueryResult result;
  result.criteria_nodes = source_nodes.nodes;
  result.criteria_nodes.insert(target_nodes.nodes.begin(),
                               target_nodes.nodes.end());
  result.diagnostics = source_nodes.diagnostics;
  result.diagnostics.unresolved_criteria.insert(
      result.diagnostics.unresolved_criteria.end(),
      target_nodes.diagnostics.unresolved_criteria.begin(),
      target_nodes.diagnostics.unresolved_criteria.end());

  if (best_target == nullptr)
    return result;

  Node *cursor = best_target;
  std::vector<Node *> path_nodes;
  std::vector<EdgeType> path_edges;
  std::unordered_set<Node *> seen;
  while (cursor != nullptr && seen.insert(cursor).second) {
    path_nodes.push_back(cursor);
    if (source_nodes.nodes.count(cursor) != 0)
      break;
    if (preds[cursor].empty())
      break;
    path_edges.push_back(preds[cursor].front().second);
    result.predecessors[cursor].insert(preds[cursor].front().first);
    cursor = preds[cursor].front().first;
  }
  std::reverse(path_nodes.begin(), path_nodes.end());
  std::reverse(path_edges.begin(), path_edges.end());

  result.nodes.insert(path_nodes.begin(), path_nodes.end());
  result.edges = collectInducedEdges(result.nodes, edge_types);
  result.distances[best_target] = best_distance;
  if (options.explain) {
    PDGWitnessPath witness;
    witness.kind = PDGWitnessPathKind::ShortestPath;
    witness.nodes = path_nodes;
    witness.edge_types = path_edges;
    result.witness_paths.push_back(witness);
  }
  return result;
}

std::vector<PDGWitnessPath> DependenceQuery::allShortestPaths(
    const PDGCriteria &sources, const PDGCriteria &targets,
    const PDGQueryOptions &options, const Module *module) const {
  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult source_nodes = resolver.resolve(sources, options, module);
  PDGQueryResult target_nodes = resolver.resolve(targets, options, module);
  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);

  std::queue<Node *> worklist;
  std::unordered_map<Node *, size_t> distances;
  std::unordered_map<Node *, std::vector<std::pair<Node *, EdgeType>>> preds;

  for (NodeSet::const_iterator it = source_nodes.nodes.begin();
       it != source_nodes.nodes.end(); ++it) {
    worklist.push(*it);
    distances[*it] = 0;
  }

  size_t best_distance = static_cast<size_t>(-1);
  std::vector<Node *> best_targets;

  while (!worklist.empty()) {
    Node *current = worklist.front();
    worklist.pop();
    const size_t distance_to_current = distances[current];
    if (distance_to_current >= best_distance)
      continue;

    for (Node::EdgeSet::const_iterator it = current->getOutEdgeSet().begin();
         it != current->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr)
        continue;
      if (!scoped_nodes.empty() && scoped_nodes.count(neighbor) == 0)
        continue;

      const size_t next_distance = distance_to_current + 1;
      std::unordered_map<Node *, size_t>::iterator dist_it =
          distances.find(neighbor);
      if (dist_it == distances.end()) {
        distances[neighbor] = next_distance;
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
        worklist.push(neighbor);
      } else if (next_distance == dist_it->second) {
        preds[neighbor].push_back(std::make_pair(current, edge->getEdgeType()));
      }

      if (target_nodes.nodes.count(neighbor) != 0) {
        if (next_distance < best_distance) {
          best_distance = next_distance;
          best_targets.clear();
          best_targets.push_back(neighbor);
        } else if (next_distance == best_distance) {
          best_targets.push_back(neighbor);
        }
      }
    }
  }

  std::vector<PDGWitnessPath> results;
  std::set<std::string> seen_paths;
  std::function<void(Node *, std::vector<Node *> &, std::vector<EdgeType> &)>
      build = [&](Node *current, std::vector<Node *> &nodes,
                  std::vector<EdgeType> &edges) {
        if (source_nodes.nodes.count(current) != 0) {
          PDGWitnessPath witness;
          witness.kind = PDGWitnessPathKind::AllShortestPath;
          witness.nodes.assign(nodes.rbegin(), nodes.rend());
          witness.edge_types.assign(edges.rbegin(), edges.rend());
          const std::string key = pathKey(witness);
          if (seen_paths.insert(key).second)
            results.push_back(witness);
          return;
        }

        std::vector<std::pair<Node *, EdgeType>> &pred_list = preds[current];
        for (size_t i = 0; i < pred_list.size(); ++i) {
          nodes.push_back(pred_list[i].first);
          edges.push_back(pred_list[i].second);
          build(pred_list[i].first, nodes, edges);
          edges.pop_back();
          nodes.pop_back();
          if (options.limits.max_paths > 0 &&
              results.size() >= options.limits.max_paths)
            return;
        }
      };

  for (size_t i = 0; i < best_targets.size(); ++i) {
    std::vector<Node *> nodes;
    std::vector<EdgeType> edges;
    nodes.push_back(best_targets[i]);
    build(best_targets[i], nodes, edges);
    if (options.limits.max_paths > 0 &&
        results.size() >= options.limits.max_paths)
      break;
  }

  return results;
}

size_t DependenceQuery::distance(const PDGCriteria &sources,
                                 const PDGCriteria &targets,
                                 const PDGQueryOptions &options,
                                 const Module *module) const {
  PDGQueryResult result = shortestPath(sources, targets, options, module);
  if (result.witness_paths.empty())
    return static_cast<size_t>(-1);
  return result.witness_paths.front().nodes.size() - 1;
}

} // namespace pdg
