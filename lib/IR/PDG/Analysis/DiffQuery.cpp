#include "IR/PDG/Analysis/DiffQuery.h"

#include "IR/PDG/Analysis/Internal/QuerySupport.h"
#include "IR/PDG/Analysis/Query.h"

#include <unordered_set>

namespace pdg {
using namespace llvm;
using namespace query_detail;

bool DiffQueryResult::isIdentical() const {
  for (size_t i = 0; i < node_diffs.size(); ++i) {
    if (node_diffs[i].kind != DiffKind::Preserved)
      return false;
  }
  for (size_t i = 0; i < edge_diffs.size(); ++i) {
    if (edge_diffs[i].kind != DiffKind::Preserved)
      return false;
  }
  return true;
}

static bool nodesMatch(Node *lhs, Node *rhs, NodeMatchStrategy strategy) {
  if (strategy == NodeMatchStrategy::PointerIdentity)
    return lhs == rhs;
  return stableNodeKey(lhs) == stableNodeKey(rhs);
}

DiffQueryResult DiffQuery::diff(const PDGQueryResult &before,
                                const PDGQueryResult &after,
                                const PDGQueryOptions &options) const {
  DiffQueryResult result;
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);
  EdgeSet before_edges = collectInducedEdges(before.nodes, edge_types);
  EdgeSet after_edges = collectInducedEdges(after.nodes, edge_types);

  std::unordered_set<Node *> matched_after_nodes;
  for (NodeSet::const_iterator it = before.nodes.begin();
       it != before.nodes.end(); ++it) {
    Node *node = *it;
    bool matched = false;
    for (NodeSet::const_iterator jt = after.nodes.begin();
         jt != after.nodes.end(); ++jt) {
      if (matched_after_nodes.count(*jt) != 0)
        continue;
      if (nodesMatch(node, *jt, strategy_)) {
        matched = true;
        matched_after_nodes.insert(*jt);
        break;
      }
    }
    result.node_diffs.push_back(
        NodeDiffEntry{node, matched ? DiffKind::Preserved : DiffKind::Removed});
  }
  for (NodeSet::const_iterator it = after.nodes.begin();
       it != after.nodes.end(); ++it) {
    if (matched_after_nodes.count(*it) == 0)
      result.node_diffs.push_back(NodeDiffEntry{*it, DiffKind::Added});
  }

  std::unordered_set<Edge *> matched_after_edges;
  for (EdgeSet::const_iterator it = before_edges.begin();
       it != before_edges.end(); ++it) {
    Edge *edge = *it;
    bool matched = false;
    for (EdgeSet::const_iterator jt = after_edges.begin();
         jt != after_edges.end(); ++jt) {
      Edge *candidate = *jt;
      if (matched_after_edges.count(candidate) != 0)
        continue;
      if (edge->getEdgeType() == candidate->getEdgeType() &&
          nodesMatch(edge->getSrcNode(), candidate->getSrcNode(), strategy_) &&
          nodesMatch(edge->getDstNode(), candidate->getDstNode(), strategy_)) {
        matched = true;
        matched_after_edges.insert(candidate);
        break;
      }
    }
    result.edge_diffs.push_back(
        EdgeDiffEntry{edge, matched ? DiffKind::Preserved : DiffKind::Removed});
  }
  for (EdgeSet::const_iterator it = after_edges.begin();
       it != after_edges.end(); ++it) {
    if (matched_after_edges.count(*it) == 0)
      result.edge_diffs.push_back(EdgeDiffEntry{*it, DiffKind::Added});
  }

  for (size_t i = 0; i < result.node_diffs.size(); ++i) {
    if (result.node_diffs[i].kind == DiffKind::Preserved)
      continue;
    const std::string function = functionNameForNode(result.node_diffs[i].node);
    if (!function.empty())
      result.impact_summary.functions[function]++;
    const std::string source = sourceKeyForNode(result.node_diffs[i].node);
    if (!source.empty())
      result.impact_summary.source_locations[source]++;
  }

  return result;
}

DiffQueryResult DiffQuery::diff(const PDGQueryScope &before,
                                const PDGQueryScope &after,
                                const PDGQueryOptions &options) const {
  PDGQueryResult before_result;
  before_result.nodes = scopeNodes(pdg_, before);
  before_result.edges = collectInducedEdges(
      before_result.nodes, edgeTypesForPreset(options.edge_preset));
  PDGQueryResult after_result;
  after_result.nodes = scopeNodes(pdg_, after);
  after_result.edges = collectInducedEdges(
      after_result.nodes, edgeTypesForPreset(options.edge_preset));
  return diff(before_result, after_result, options);
}

} // namespace pdg
