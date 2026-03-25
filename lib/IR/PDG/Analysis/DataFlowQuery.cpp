/**
 * @file DataFlowQuery.cpp
 * @brief Implementation of classic data-flow analyses over the PDG.
 *
 * References:
 * - Kuck, Kuhn, Padua, Leasure & Wolfe, "Dependence Graphs and Compiler
 *   Optimizations", POPL 1981.
 * - Ferrante, Ottenstein & Warren, "The Program Dependence Graph and Its Use
 *   in Optimization", TOPLAS 1987.
 * - Denning & Denning, "Certification of Programs for Secure Information
 *   Flow", CACM 1977.
 * - Aho, Sethi, Ullman, "Compilers: Principles, Techniques, and Tools".
 */

#include "IR/PDG/Analysis/DataFlowQuery.h"

#include <algorithm>
#include <queue>

using namespace llvm;

namespace pdg {

// ============================================================================
// Shared helpers
// ============================================================================

namespace {

/// Data-dependence edge types used across several analyses.
std::set<EdgeType> dataEdgeTypes() {
  return {EdgeType::DATA_DEF_USE,   EdgeType::DATA_RAW,
          EdgeType::DATA_READ,      EdgeType::DATA_ALIAS,
          EdgeType::DATA_RET,       EdgeType::VAL_DEP,
          EdgeType::PARAMETER_IN,   EdgeType::PARAMETER_OUT,
          EdgeType::PARAMETER_FIELD};
}

/// Control-dependence edge types.
std::set<EdgeType> controlEdgeTypes() {
  return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
          EdgeType::CONTROLDEP_ENTRY, EdgeType::CONTROLDEP_BR,
          EdgeType::CONTROLDEP_IND_BR};
}

bool isEdgeAllowed(EdgeType et, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(et);
}

/// Generic BFS collecting reachable nodes.
template <typename GetEdgesFunc, typename GetNeighborFunc>
std::set<Node *> bfsCollect(Node &start, const std::set<EdgeType> &edge_types,
                            size_t max_depth, GetEdgesFunc get_edges,
                            GetNeighborFunc get_neighbor) {
  std::set<Node *> result;
  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist;

  worklist.push({&start, 0});
  visited.insert(&start);
  result.insert(&start);

  while (!worklist.empty()) {
    auto front = worklist.front();
    Node *current = front.first;
    size_t depth = front.second;
    worklist.pop();

    if (max_depth > 0 && depth >= max_depth)
      continue;

    for (auto *edge : get_edges(current)) {
      if (edge == nullptr)
        continue;
      if (!isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;

      Node *neighbor = get_neighbor(edge);
      if (neighbor == nullptr || visited.count(neighbor))
        continue;

      visited.insert(neighbor);
      result.insert(neighbor);
      worklist.push({neighbor, depth + 1});
    }
  }

  return result;
}

} // namespace

// ============================================================================
// ReachingDefinitions
// ============================================================================

std::set<EdgeType> ReachingDefinitions::getDataEdgeTypes() {
  return dataEdgeTypes();
}

std::vector<ReachingDef> ReachingDefinitions::directDefs(Node &use_node) {
  std::vector<ReachingDef> result;
  auto data_edges = getDataEdgeTypes();

  for (auto *edge : use_node.getInEdgeSet()) {
    if (edge == nullptr)
      continue;
    if (!data_edges.count(edge->getEdgeType()))
      continue;

    Node *def = edge->getSrcNode();
    if (def != nullptr)
      result.push_back({def, edge->getEdgeType()});
  }

  return result;
}

ReachingDefinitions::NodeSet
ReachingDefinitions::transitiveDefs(Node &use_node, size_t max_depth) {
  auto data_edges = getDataEdgeTypes();
  auto all = bfsCollect(
      use_node, data_edges, max_depth,
      [](Node *n) -> Node::EdgeSet & { return n->getInEdgeSet(); },
      [](Edge *e) { return e->getSrcNode(); });
  // Remove the seed node itself.
  all.erase(&use_node);
  return all;
}

ReachingDefinitions::NodeSet ReachingDefinitions::directUses(Node &def_node) {
  NodeSet result;
  auto data_edges = getDataEdgeTypes();

  for (auto *edge : def_node.getOutEdgeSet()) {
    if (edge == nullptr)
      continue;
    if (!data_edges.count(edge->getEdgeType()))
      continue;

    Node *use = edge->getDstNode();
    if (use != nullptr)
      result.insert(use);
  }

  return result;
}

ReachingDefinitions::NodeSet
ReachingDefinitions::transitiveUses(Node &def_node, size_t max_depth) {
  auto data_edges = getDataEdgeTypes();
  auto all = bfsCollect(
      def_node, data_edges, max_depth,
      [](Node *n) -> Node::EdgeSet & { return n->getOutEdgeSet(); },
      [](Edge *e) { return e->getDstNode(); });
  all.erase(&def_node);
  return all;
}

// ============================================================================
// DefUseChains
// ============================================================================

DefUseChains::Chain DefUseChains::getDefUseChain(Node &def_node,
                                                 size_t max_depth) {
  Chain chain;
  auto data_edges = dataEdgeTypes();
  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist;

  worklist.push({&def_node, 0});
  visited.insert(&def_node);

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();

    if (max_depth > 0 && depth >= max_depth)
      continue;

    for (auto *edge : current->getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!data_edges.count(edge->getEdgeType()))
        continue;

      Node *use = edge->getDstNode();
      if (use == nullptr || visited.count(use))
        continue;

      chain.push_back({current, use, edge->getEdgeType()});
      visited.insert(use);
      worklist.push({use, depth + 1});
    }
  }

  return chain;
}

DefUseChains::Chain DefUseChains::getUseDefChain(Node &use_node,
                                                 size_t max_depth) {
  Chain chain;
  auto data_edges = dataEdgeTypes();
  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist;

  worklist.push({&use_node, 0});
  visited.insert(&use_node);

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();

    if (max_depth > 0 && depth >= max_depth)
      continue;

    for (auto *edge : current->getInEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!data_edges.count(edge->getEdgeType()))
        continue;

      Node *def = edge->getSrcNode();
      if (def == nullptr || visited.count(def))
        continue;

      chain.push_back({def, current, edge->getEdgeType()});
      visited.insert(def);
      worklist.push({def, depth + 1});
    }
  }

  return chain;
}

std::unordered_map<Node *, DefUseChains::Chain>
DefUseChains::allDefUseChains(const NodeSet &nodes, size_t max_depth) {
  std::unordered_map<Node *, Chain> result;
  auto data_edges = dataEdgeTypes();

  for (Node *n : nodes) {
    if (n == nullptr)
      continue;

    // A node is a "definition" if it has outgoing data edges.
    bool has_data_out = false;
    for (auto *edge : n->getOutEdgeSet()) {
      if (edge && data_edges.count(edge->getEdgeType())) {
        has_data_out = true;
        break;
      }
    }

    if (has_data_out)
      result[n] = getDefUseChain(*n, max_depth);
  }

  return result;
}

// ============================================================================
// LiveVariables
// ============================================================================

std::set<EdgeType> LiveVariables::getDataEdgeTypes() { return dataEdgeTypes(); }

bool LiveVariables::isLive(Node &node) {
  auto data_edges = getDataEdgeTypes();
  for (auto *edge : node.getOutEdgeSet()) {
    if (edge == nullptr)
      continue;
    if (data_edges.count(edge->getEdgeType()))
      return true;
  }
  return false;
}

LiveVariables::NodeSet LiveVariables::allLiveNodes() {
  NodeSet result;
  for (auto it = _pdg.begin(); it != _pdg.end(); ++it) {
    Node *n = *it;
    if (n != nullptr && isLive(*n))
      result.insert(n);
  }
  return result;
}

LiveVariables::NodeSet LiveVariables::liveNodesIn(const NodeSet &subgraph) {
  NodeSet result;
  auto data_edges = getDataEdgeTypes();

  for (Node *n : subgraph) {
    if (n == nullptr)
      continue;
    for (auto *edge : n->getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!data_edges.count(edge->getEdgeType()))
        continue;
      // The use must also be in the sub-graph.
      Node *use = edge->getDstNode();
      if (use && subgraph.count(use)) {
        result.insert(n);
        break;
      }
    }
  }

  return result;
}

LiveVariables::NodeSet LiveVariables::deadNodesIn(const NodeSet &subgraph) {
  auto live = liveNodesIn(subgraph);
  NodeSet dead;
  for (Node *n : subgraph) {
    if (n && live.count(n) == 0)
      dead.insert(n);
  }
  return dead;
}

// ============================================================================
// DataOnlySlicing
// ============================================================================

std::set<EdgeType> DataOnlySlicing::getDataEdgeTypes() {
  return dataEdgeTypes();
}

DataOnlySlicing::NodeSet DataOnlySlicing::forwardSlice(Node &start_node,
                                                       size_t max_depth) {
  return forwardSlice({&start_node}, max_depth);
}

DataOnlySlicing::NodeSet
DataOnlySlicing::forwardSlice(const NodeSet &start_nodes, size_t max_depth) {
  auto data_edges = getDataEdgeTypes();
  NodeSet result;
  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist;

  for (auto *n : start_nodes) {
    if (n) {
      worklist.push({n, 0});
      visited.insert(n);
      result.insert(n);
    }
  }

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();

    if (max_depth > 0 && depth >= max_depth)
      continue;

    for (auto *edge : current->getOutEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!data_edges.count(edge->getEdgeType()))
        continue;

      Node *neighbor = edge->getDstNode();
      if (neighbor == nullptr || visited.count(neighbor))
        continue;

      visited.insert(neighbor);
      result.insert(neighbor);
      worklist.push({neighbor, depth + 1});
    }
  }

  return result;
}

DataOnlySlicing::NodeSet DataOnlySlicing::backwardSlice(Node &end_node,
                                                        size_t max_depth) {
  return backwardSlice({&end_node}, max_depth);
}

DataOnlySlicing::NodeSet
DataOnlySlicing::backwardSlice(const NodeSet &end_nodes, size_t max_depth) {
  auto data_edges = getDataEdgeTypes();
  NodeSet result;
  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist;

  for (auto *n : end_nodes) {
    if (n) {
      worklist.push({n, 0});
      visited.insert(n);
      result.insert(n);
    }
  }

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t depth = current_pair.second;
    worklist.pop();

    if (max_depth > 0 && depth >= max_depth)
      continue;

    for (auto *edge : current->getInEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!data_edges.count(edge->getEdgeType()))
        continue;

      Node *neighbor = edge->getSrcNode();
      if (neighbor == nullptr || visited.count(neighbor))
        continue;

      visited.insert(neighbor);
      result.insert(neighbor);
      worklist.push({neighbor, depth + 1});
    }
  }

  return result;
}

// ============================================================================
// ControlDependenceQuery
// ============================================================================

std::set<EdgeType> ControlDependenceQuery::getControlEdgeTypes() {
  return controlEdgeTypes();
}

std::vector<ControllingCondition>
ControlDependenceQuery::immediateControllers(Node &node) {
  std::vector<ControllingCondition> result;
  auto ctrl_edges = getControlEdgeTypes();

  for (auto *edge : node.getInEdgeSet()) {
    if (edge == nullptr)
      continue;
    if (!ctrl_edges.count(edge->getEdgeType()))
      continue;

    Node *pred = edge->getSrcNode();
    if (pred != nullptr)
      result.push_back({pred, edge->getEdgeType()});
  }

  return result;
}

ControlDependenceQuery::NodeSet
ControlDependenceQuery::allControllers(Node &node, size_t max_depth) {
  auto ctrl_edges = getControlEdgeTypes();
  auto all = bfsCollect(
      node, ctrl_edges, max_depth,
      [](Node *n) -> Node::EdgeSet & { return n->getInEdgeSet(); },
      [](Edge *e) { return e->getSrcNode(); });
  all.erase(&node);
  return all;
}

ControlDependenceQuery::NodeSet
ControlDependenceQuery::controlledBy(Node &predicate_node) {
  NodeSet result;
  auto ctrl_edges = getControlEdgeTypes();

  for (auto *edge : predicate_node.getOutEdgeSet()) {
    if (edge == nullptr)
      continue;
    if (!ctrl_edges.count(edge->getEdgeType()))
      continue;

    Node *dep = edge->getDstNode();
    if (dep != nullptr)
      result.insert(dep);
  }

  return result;
}

ControlDependenceQuery::NodeSet
ControlDependenceQuery::controlRegion(Node &predicate_node, size_t max_depth) {
  auto ctrl_edges = getControlEdgeTypes();
  auto all = bfsCollect(
      predicate_node, ctrl_edges, max_depth,
      [](Node *n) -> Node::EdgeSet & { return n->getOutEdgeSet(); },
      [](Edge *e) { return e->getDstNode(); });
  all.erase(&predicate_node);
  return all;
}

size_t ControlDependenceQuery::nestingDepth(Node &node) {
  auto ctrl_edges = getControlEdgeTypes();
  if (node.getNodeType() == GraphNodeType::FUNC_ENTRY)
    return 0;

  std::unordered_set<Node *> visited;
  std::queue<std::pair<Node *, size_t>> worklist;

  worklist.push({&node, 0});
  visited.insert(&node);

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    size_t d = current_pair.second;
    worklist.pop();

    // First reachable FUNC_ENTRY in BFS gives the shortest distance.
    if (current->getNodeType() == GraphNodeType::FUNC_ENTRY)
      return d;

    for (auto *edge : current->getInEdgeSet()) {
      if (edge == nullptr)
        continue;
      if (!ctrl_edges.count(edge->getEdgeType()))
        continue;

      Node *pred = edge->getSrcNode();
      if (pred == nullptr || visited.count(pred))
        continue;

      visited.insert(pred);
      worklist.push({pred, d + 1});
    }
  }

  // No reachable entry via control-dependence edges.
  return 0;
}

} // namespace pdg
