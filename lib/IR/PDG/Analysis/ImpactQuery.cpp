/**
 * @file ImpactQuery.cpp
 * @brief Implementation of PDG impact analysis.
 *
 * ImpactQuery layers user-facing "what is affected?" semantics on top of the
 * core slice, distance, summary, and diff services. The implementation here is
 * intentionally compositional: it reuses existing query primitives rather than
 * duplicating traversal logic.
 */

#include "IR/PDG/Analysis/ImpactQuery.h"

#include "IR/PDG/Analysis/PDGQuery.h"
#include "IR/PDG/Analysis/SummaryQuery.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>

using namespace llvm;

namespace pdg {

namespace {

using NodeSet = PDGQueryResult::NodeSet;

static std::string functionNameForNode(Node *node) {
  if (node == nullptr)
    return "";
  if (node->getFunc() != nullptr)
    return node->getFunc()->getName().str();
  const Function *function = dyn_cast_or_null<Function>(node->getValue());
  return function ? function->getName().str() : "";
}

static std::string sourceKeyForNode(Node *node) {
  if (node == nullptr)
    return "";
  const Instruction *inst = dyn_cast_or_null<Instruction>(node->getValue());
  if (inst == nullptr || !inst->getDebugLoc())
    return "";
  DebugLoc loc = inst->getDebugLoc();
  std::string key = loc->getFilename().str();
  key += ":";
  key += std::to_string(loc.getLine());
  key += ":";
  key += std::to_string(loc.getCol());
  return key;
}

/// Count interprocedural boundaries crossed by a witness path.
static size_t countInterproceduralCrossings(const std::vector<EdgeType> &edges) {
  size_t count = 0;
  for (size_t i = 0; i < edges.size(); ++i) {
    const EdgeType type = edges[i];
    if (type == EdgeType::CONTROLDEP_CALLINV ||
        type == EdgeType::CONTROLDEP_CALLRET ||
        type == EdgeType::PARAMETER_IN || type == EdgeType::PARAMETER_OUT ||
        type == EdgeType::PARAMETER_FIELD || type == EdgeType::DATA_RET)
      ++count;
  }
  return count;
}

static std::string calleeName(Node *node) {
  if (node == nullptr || node->getNodeType() != GraphNodeType::INST_FUNCALL)
    return "";
  const CallBase *call = dyn_cast_or_null<CallBase>(node->getValue());
  if (call == nullptr || call->getCalledFunction() == nullptr)
    return "";
  return call->getCalledFunction()->getName().str();
}

/// Aggregate impacted function names from a node-level query result.
static std::vector<std::string> impactedFunctionsFromResult(
    const PDGQueryResult &result) {
  std::set<std::string> names;
  for (NodeSet::const_iterator it = result.nodes.begin(); it != result.nodes.end();
       ++it) {
    const std::string name = functionNameForNode(*it);
    if (!name.empty())
      names.insert(name);
  }
  return std::vector<std::string>(names.begin(), names.end());
}

static NodeSet scopeNodes(ProgramGraph &pdg, const PDGQueryScope &scope) {
  NodeSet nodes;
  if (scope.kind == PDGQueryScope::Kind::WholeGraph) {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it)
      if (*it != nullptr)
        nodes.insert(*it);
    return nodes;
  }
  if (scope.kind == PDGQueryScope::Kind::NodeSet)
    return scope.nodes;
  if (scope.kind == PDGQueryScope::Kind::QueryResult && scope.query_result)
    return scope.query_result->nodes;
  if (scope.kind == PDGQueryScope::Kind::Function && scope.function) {
    for (ProgramGraph::NodeSet::iterator it = pdg.begin(); it != pdg.end(); ++it) {
      Node *node = *it;
      if (node != nullptr && node->getFunc() == scope.function)
        nodes.insert(node);
    }
  }
  return nodes;
}

static bool isEdgeAllowed(EdgeType type, const std::set<EdgeType> &allowed) {
  return allowed.empty() || allowed.count(type) != 0;
}

} // namespace

ImpactQueryResult ImpactQuery::analyze(const PDGCriteria &criteria,
                                       const ImpactPolicy &policy,
                                       const PDGQueryOptions &options,
                                       const Module *module) const {
  // Start with criteria resolution, then build direct and transitive views
  // from the shared PDG traversal services.
  ImpactQueryResult result;
  PDGCriteriaResolver resolver(pdg_);
  PDGQueryResult resolved = resolver.resolve(criteria, options, module);
  result.diagnostics = resolved.diagnostics;

  PDGQueryResult direct;
  direct.criteria_nodes = resolved.criteria_nodes;
  const NodeSet scoped_nodes = scopeNodes(pdg_, options.scope);
  const std::set<EdgeType> edge_types = edgeTypesForPreset(options.edge_preset);
  for (NodeSet::const_iterator it = resolved.nodes.begin(); it != resolved.nodes.end();
       ++it) {
    Node *node = *it;
    direct.nodes.insert(node);
    for (Node::EdgeSet::const_iterator edge_it = node->getOutEdgeSet().begin();
         edge_it != node->getOutEdgeSet().end(); ++edge_it) {
      Edge *edge = *edge_it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *dst = edge->getDstNode();
      if (dst == nullptr)
        continue;
      if (!scoped_nodes.empty() && scoped_nodes.count(dst) == 0)
        continue;
      direct.nodes.insert(dst);
      direct.edges.insert(edge);
      direct.predecessors[dst].insert(node);
      if (direct.distances.count(dst) == 0)
        direct.distances[dst] = 1;
    }
  }
  result.directly_impacted_nodes = direct;

  SliceQuery slice_query(pdg_);
  result.transitively_impacted_nodes =
      slice_query.forward(criteria, options, module);

  std::vector<std::string> functions =
      impactedFunctionsFromResult(result.transitively_impacted_nodes);
  result.impacted_functions.insert(functions.begin(), functions.end());
  for (NodeSet::const_iterator it =
           result.transitively_impacted_nodes.nodes.begin();
       it != result.transitively_impacted_nodes.nodes.end(); ++it) {
    const std::string source = sourceKeyForNode(*it);
    if (!source.empty())
      result.impacted_source_locations.insert(source);
  }

  for (PDGQueryResult::EdgeSet::const_iterator it =
           result.transitively_impacted_nodes.edges.begin();
       it != result.transitively_impacted_nodes.edges.end(); ++it) {
    Edge *edge = *it;
    if (edge == nullptr)
      continue;
    if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLINV)
      result.boundary_crossings["call"]++;
    else if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLRET ||
             edge->getEdgeType() == EdgeType::DATA_RET)
      result.boundary_crossings["return"]++;
    else if (edge->getEdgeType() == EdgeType::PARAMETER_IN ||
             edge->getEdgeType() == EdgeType::PARAMETER_OUT ||
             edge->getEdgeType() == EdgeType::PARAMETER_FIELD)
      result.boundary_crossings["parameter"]++;
  }

  SummaryQuery summary_query(pdg_);
  for (std::set<std::string>::const_iterator it = result.impacted_functions.begin();
       it != result.impacted_functions.end(); ++it) {
    if (module == nullptr)
      continue;
    const Function *function = module->getFunction(*it);
    if (function == nullptr)
      continue;
    SummaryQueryResult summary =
        summary_query.summarize(*function, SummaryPolicy(), options, module);
    for (size_t j = 0; j < summary.summary.reachable_calls.size(); ++j) {
      Node *call_node = summary.summary.reachable_calls[j].target;
      const std::string callee = calleeName(call_node);
      if (!callee.empty())
        result.impacted_functions.insert(callee);
      result.function_explanations[*it].insert(
          result.function_explanations[*it].end(),
          summary.summary.reachable_calls[j].witness_paths.begin(),
          summary.summary.reachable_calls[j].witness_paths.end());
    }
  }

  for (NodeSet::const_iterator it =
           result.transitively_impacted_nodes.nodes.begin();
       it != result.transitively_impacted_nodes.nodes.end(); ++it) {
    Node *node = *it;
    if (resolved.nodes.count(node) != 0)
      continue;
    ImpactRankItem item;
    item.node = node;
    item.stable_key = stableNodeKey(node);
    std::unordered_map<Node *, size_t>::const_iterator dist_it =
        result.transitively_impacted_nodes.distances.find(node);
    item.shortest_distance =
        dist_it == result.transitively_impacted_nodes.distances.end()
            ? 0
            : dist_it->second;
    size_t path_count = 0;
    size_t crossings = 0;
    for (size_t i = 0; i < result.transitively_impacted_nodes.witness_paths.size();
         ++i) {
      const PDGWitnessPath &path = result.transitively_impacted_nodes.witness_paths[i];
      if (!path.nodes.empty() && path.nodes.back() == node) {
        ++path_count;
        crossings =
            std::max(crossings, countInterproceduralCrossings(path.edge_types));
      }
    }
    item.path_count = path_count == 0 ? 1 : path_count;
    item.interprocedural_crossings = crossings;
    result.ranked_impacts.push_back(item);
  }

  std::sort(result.ranked_impacts.begin(), result.ranked_impacts.end(),
            [](const ImpactRankItem &lhs, const ImpactRankItem &rhs) {
              if (lhs.shortest_distance != rhs.shortest_distance)
                return lhs.shortest_distance < rhs.shortest_distance;
              if (lhs.interprocedural_crossings != rhs.interprocedural_crossings)
                return lhs.interprocedural_crossings <
                       rhs.interprocedural_crossings;
              if (lhs.path_count != rhs.path_count)
                return lhs.path_count > rhs.path_count;
              return lhs.stable_key < rhs.stable_key;
            });
  if (policy.max_ranked_impacts > 0 &&
      result.ranked_impacts.size() > policy.max_ranked_impacts)
    result.ranked_impacts.resize(policy.max_ranked_impacts);
  return result;
}

ImpactQueryResult
ImpactQuery::analyzeAgainstBaseline(const PDGCriteria &criteria,
                                    const PDGCriteria &baseline_criteria,
                                    const ImpactPolicy &policy,
                                    const PDGQueryOptions &options,
                                    const Module *module) const {
  ImpactQueryResult result = analyze(criteria, policy, options, module);
  ImpactQueryResult baseline = analyze(baseline_criteria, policy, options, module);
  DiffQuery diff_query(pdg_);
  result.changed_only_diff =
      diff_query.diff(baseline.transitively_impacted_nodes,
                      result.transitively_impacted_nodes, options);
  if (policy.changed_only) {
    std::set<Node *> changed_nodes;
    for (size_t i = 0; i < result.changed_only_diff.node_diffs.size(); ++i) {
      if (result.changed_only_diff.node_diffs[i].kind != DiffKind::Preserved &&
          result.changed_only_diff.node_diffs[i].node != nullptr)
        changed_nodes.insert(result.changed_only_diff.node_diffs[i].node);
    }
    std::vector<ImpactRankItem> filtered;
    for (size_t i = 0; i < result.ranked_impacts.size(); ++i) {
      if (changed_nodes.count(result.ranked_impacts[i].node) != 0)
        filtered.push_back(result.ranked_impacts[i]);
    }
    if (filtered.empty()) {
      for (NodeSet::const_iterator it =
               result.transitively_impacted_nodes.criteria_nodes.begin();
           it != result.transitively_impacted_nodes.criteria_nodes.end(); ++it) {
        if (changed_nodes.count(*it) == 0)
          continue;
        ImpactRankItem item;
        item.node = *it;
        item.stable_key = stableNodeKey(*it);
        result.ranked_impacts.push_back(item);
      }
    } else {
      result.ranked_impacts.swap(filtered);
    }
  }
  result.diagnostics.notes.push_back("Computed changed-only impact diff");
  return result;
}

} // namespace pdg
