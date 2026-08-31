/**
 * @file GraphView.h
 * @brief Non-owning filtered views over Lotus ICFG and SVFG graphs.
 */
#pragma once

#include "IR/ICFG/ICFG.h"
#include "IR/SVFG/SVFG.h"

#include <cstddef>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::analysis {

/// A non-owning induced subgraph. Nodes and edges remain owned by the source
/// graph; an edge is visible exactly when both endpoints are retained.
template <typename GraphT, typename NodeT, typename EdgeT>
class FilteredGraphView {
public:
  using NodeSet = std::unordered_set<const NodeT *>;

  FilteredGraphView(const GraphT &graph, NodeSet retained)
      : graph_(&graph), retained_(std::move(retained)) {}

  template <typename Predicate>
  FilteredGraphView(const GraphT &graph, Predicate keep) : graph_(&graph) {
    for (const auto &entry : graph) {
      const NodeT *node = entry.second;
      if (node && keep(*node))
        retained_.insert(node);
    }
  }

  const GraphT &source() const { return *graph_; }

  bool contains(const NodeT *node) const {
    return node && retained_.count(node) != 0;
  }

  bool contains(const EdgeT *edge) const {
    return edge && contains(edge->getSrcNode()) && contains(edge->getDstNode());
  }

  std::size_t nodeCount() const { return retained_.size(); }

  std::size_t edgeCount() const {
    std::size_t count = 0;
    for (const NodeT *node : retained_)
      for (const EdgeT *edge : node->getOutEdges())
        count += contains(edge) ? 1U : 0U;
    return count;
  }

  const NodeSet &nodes() const { return retained_; }

  std::vector<const EdgeT *> outgoing(const NodeT *node) const {
    std::vector<const EdgeT *> result;
    if (!contains(node))
      return result;
    for (const EdgeT *edge : node->getOutEdges())
      if (contains(edge))
        result.push_back(edge);
    return result;
  }

  std::vector<const EdgeT *> incoming(const NodeT *node) const {
    std::vector<const EdgeT *> result;
    if (!contains(node))
      return result;
    for (const EdgeT *edge : node->getInEdges())
      if (contains(edge))
        result.push_back(edge);
    return result;
  }

  std::vector<const NodeT *> successors(const NodeT *node) const {
    std::vector<const NodeT *> result;
    for (const EdgeT *edge : outgoing(node))
      result.push_back(edge->getDstNode());
    return result;
  }

  std::vector<const NodeT *> predecessors(const NodeT *node) const {
    std::vector<const NodeT *> result;
    for (const EdgeT *edge : incoming(node))
      result.push_back(edge->getSrcNode());
    return result;
  }

private:
  const GraphT *graph_;
  NodeSet retained_;
};

using FilteredICFGView = FilteredGraphView<ICFG, ICFGNode, ICFGEdge>;
using FilteredSVFGView = FilteredGraphView<SVFG, SVFGNode, SVFGEdge>;

} // namespace lotus::analysis
