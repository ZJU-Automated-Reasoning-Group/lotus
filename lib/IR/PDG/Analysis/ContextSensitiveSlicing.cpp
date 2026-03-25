/**
 * @file ContextSensitiveSlicing.cpp
 * @brief Implementation of context-sensitive slicing (tabulation-style
 * CFL-reachability)
 *
 * Implements context-sensitive slicing by tabulation over the PDG: valid paths
 * are those with properly matched call/return pairs (CFL-reachability).
 * Optional procedure summary caching avoids re-exploring the same callee for
 * the same caller context ("summary edges at callee"), matching IFDS-based
 * slicer designs.
 */

#include "IR/PDG/Analysis/ContextSensitiveSlicing.h"

#include "IR/PDG/Support/PDGUtils.h"

#include <algorithm>
#include <queue>

using namespace llvm;

namespace pdg {

namespace {
bool applyCallStackTransition(bool forward, Node *current, Node *neighbor,
                              EdgeType edge_type,
                              std::vector<Node *> &call_stack) {
  if (edge_type == EdgeType::CONTROLDEP_CALLINV) {
    if (forward) {
      call_stack.push_back(current);
      return true;
    }
    if (call_stack.empty() || neighbor != call_stack.back())
      return false;
    call_stack.pop_back();
    return true;
  }

  if (edge_type == EdgeType::CONTROLDEP_CALLRET) {
    if (forward) {
      if (call_stack.empty() || neighbor != call_stack.back())
        return false;
      call_stack.pop_back();
      return true;
    }
    // Backward traversal follows the reversed return edge from the callsite
    // into the callee, so the callsite itself is the stack token.
    call_stack.push_back(current);
    return true;
  }

  return true;
}
} // namespace

// ==================== SliceOptions ====================

std::set<EdgeType> SliceOptions::getEdgeTypes() const {
  std::set<EdgeType> types;
  if (include_data_deps) {
    types.insert(EdgeType::DATA_DEF_USE);
    types.insert(EdgeType::DATA_RAW);
    types.insert(EdgeType::DATA_READ);
    types.insert(EdgeType::DATA_ALIAS);
    types.insert(EdgeType::DATA_RET);
    types.insert(EdgeType::VAL_DEP);
    types.insert(EdgeType::GLOBAL_DEP);
  }
  if (include_control_deps) {
    types.insert(EdgeType::CONTROLDEP_BR);
    types.insert(EdgeType::CONTROLDEP_IND_BR);
    types.insert(EdgeType::CONTROLDEP_ENTRY);
  }
  if (include_param_edges) {
    types.insert(EdgeType::PARAMETER_IN);
    types.insert(EdgeType::PARAMETER_OUT);
    types.insert(EdgeType::PARAMETER_FIELD);
    types.insert(EdgeType::DATA_RET);
  }
  if (include_call_return_edges) {
    types.insert(EdgeType::CONTROLDEP_CALLINV);
    types.insert(EdgeType::CONTROLDEP_CALLRET);
  }
  return types;
}

// ==================== ContextSensitiveSlicing Implementation
// ====================

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeForwardSlice(
    Node &start_node, const std::set<EdgeType> &edge_types) {
  return computeForwardSlice({&start_node}, edge_types);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeForwardSlice(
    Node &start_node, const std::set<EdgeType> &edge_types,
    const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics) {
  return computeForwardSlice({&start_node}, edge_types, limits, diagnostics);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeForwardSlice(
    const NodeSet &start_nodes, const std::set<EdgeType> &edge_types) {
  return traverseWithStack(start_nodes, edge_types, true);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeForwardSlice(
    const NodeSet &start_nodes, const std::set<EdgeType> &edge_types,
    const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics) {
  return traverseWithStack(start_nodes, edge_types, true, limits, diagnostics);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeBackwardSlice(
    Node &end_node, const std::set<EdgeType> &edge_types) {
  return computeBackwardSlice({&end_node}, edge_types);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeBackwardSlice(
    Node &end_node, const std::set<EdgeType> &edge_types,
    const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics) {
  return computeBackwardSlice({&end_node}, edge_types, limits, diagnostics);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeBackwardSlice(
    const NodeSet &end_nodes, const std::set<EdgeType> &edge_types) {
  return traverseWithStack(end_nodes, edge_types, false);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::computeBackwardSlice(
    const NodeSet &end_nodes, const std::set<EdgeType> &edge_types,
    const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics) {
  return traverseWithStack(end_nodes, edge_types, false, limits, diagnostics);
}

ContextSensitiveSlicing::NodeSet
ContextSensitiveSlicing::computeForwardSlice(const NodeSet &start_nodes,
                                             const SliceOptions &options,
                                             CFLDiagnostics *diagnostics) {
  CFLTraversalLimits limits;
  limits.max_states = options.max_states;
  limits.max_stack_depth = options.max_stack_depth;
  std::set<EdgeType> edge_types = options.getEdgeTypes();
  SummaryCache cache;
  return traverseWithStack(start_nodes, edge_types, true, limits, diagnostics,
                           options.use_summary_cache,
                           options.use_summary_cache ? &cache : nullptr);
}

ContextSensitiveSlicing::NodeSet
ContextSensitiveSlicing::computeBackwardSlice(const NodeSet &end_nodes,
                                              const SliceOptions &options,
                                              CFLDiagnostics *diagnostics) {
  CFLTraversalLimits limits;
  limits.max_states = options.max_states;
  limits.max_stack_depth = options.max_stack_depth;
  std::set<EdgeType> edge_types = options.getEdgeTypes();
  SummaryCache cache;
  return traverseWithStack(end_nodes, edge_types, false, limits, diagnostics,
                           options.use_summary_cache,
                           options.use_summary_cache ? &cache : nullptr);
}

ContextSensitiveSlicing::NodeSet
ContextSensitiveSlicing::computeChop(Node &source_node, Node &sink_node,
                                     const std::set<EdgeType> &edge_types) {
  NodeSet forward_slice = computeForwardSlice(source_node, edge_types);
  NodeSet backward_slice = computeBackwardSlice(sink_node, edge_types);

  NodeSet chop;
  for (auto *node : forward_slice) {
    if (backward_slice.count(node)) {
      chop.insert(node);
    }
  }
  return chop;
}

bool ContextSensitiveSlicing::hasContextSensitivePath(
    Node &source_node, Node &sink_node, const std::set<EdgeType> &edge_types) {
  return !computeChop(source_node, sink_node, edge_types).empty();
}

ContextSensitiveSlicing::NodeSet
ContextSensitiveSlicing::traverseWithStack(const NodeSet &start_nodes,
                                           const std::set<EdgeType> &edge_types,
                                           bool forward) {
  return traverseWithStack(start_nodes, edge_types, forward,
                           CFLTraversalLimits{}, nullptr, false, nullptr);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::traverseWithStack(
    const NodeSet &start_nodes, const std::set<EdgeType> &edge_types,
    bool forward, const CFLTraversalLimits &limits,
    CFLDiagnostics *diagnostics) {
  return traverseWithStack(start_nodes, edge_types, forward, limits,
                           diagnostics, false, nullptr);
}

ContextSensitiveSlicing::NodeSet ContextSensitiveSlicing::traverseWithStack(
    const NodeSet &start_nodes, const std::set<EdgeType> &edge_types,
    bool forward, const CFLTraversalLimits &limits, CFLDiagnostics *diagnostics,
    bool use_summary_cache, SummaryCache *summary_cache) {
  NodeSet slice;
  VisitedSet visited;
  std::queue<std::pair<Node *, std::vector<Node *>>> worklist;
  if (diagnostics != nullptr)
    *diagnostics = CFLDiagnostics{};

  for (auto *node : start_nodes) {
    if (node != nullptr) {
      worklist.push({node, std::vector<Node *>()});
      slice.insert(node);
    }
  }

  while (!worklist.empty()) {
    auto current_pair = worklist.front();
    Node *current = current_pair.first;
    std::vector<Node *> call_stack = current_pair.second;
    worklist.pop();

    if (diagnostics != nullptr) {
      diagnostics->max_stack_depth_reached =
          std::max(diagnostics->max_stack_depth_reached, call_stack.size());
    }
    if (limits.max_stack_depth > 0 &&
        call_stack.size() > limits.max_stack_depth) {
      if (diagnostics != nullptr)
        diagnostics->stack_depth_limit_hit = true;
      continue;
    }

    auto state = std::make_pair(current, call_stack);
    if (visited.find(state) != visited.end()) {
      continue;
    }
    if (limits.max_states > 0 && visited.size() + 1 > limits.max_states) {
      if (diagnostics != nullptr)
        diagnostics->state_limit_hit = true;
      break;
    }
    visited.insert(state);

    // Summary cache: when we enter a callee (stack = [call_site]), reuse or
    // compute summary.
    if (use_summary_cache && summary_cache != nullptr &&
        call_stack.size() == 1u &&
        current->getNodeType() == GraphNodeType::FUNC_ENTRY) {
      Node *call_site = call_stack.back();
      auto key = std::make_pair(current, call_site);
      auto it = summary_cache->find(key);
      if (it != summary_cache->end()) {
        if (diagnostics != nullptr)
          diagnostics->summary_hits++;
        for (Node *n : it->second.reachable)
          slice.insert(n);
        if (it->second.returns_to_caller) {
          auto new_state_return =
              std::make_pair(call_site, std::vector<Node *>());
          if (visited.find(new_state_return) == visited.end()) {
            slice.insert(call_site);
            worklist.push({call_site, std::vector<Node *>()});
          }
        }
        continue;
      }
      if (diagnostics != nullptr)
        diagnostics->summary_misses++;
      // Cache miss: compute summary for (current, call_site) for future hits.
      (*summary_cache)[key] =
          computeProcedureSummary(current, call_site, edge_types, forward);
    }

    auto &edges = forward ? current->getOutEdgeSet() : current->getInEdgeSet();
    for (auto *edge : edges) {
      if (edge == nullptr ||
          (!edge_types.empty() &&
           edge_types.find(edge->getEdgeType()) == edge_types.end())) {
        continue;
      }

      Node *neighbor = forward ? edge->getDstNode() : edge->getSrcNode();
      if (neighbor == nullptr) {
        continue;
      }

      std::vector<Node *> new_stack = call_stack;
      if (!applyCallStackTransition(forward, current, neighbor,
                                    edge->getEdgeType(), new_stack)) {
        continue;
      }

      if (limits.max_stack_depth > 0 &&
          new_stack.size() > limits.max_stack_depth) {
        if (diagnostics != nullptr)
          diagnostics->stack_depth_limit_hit = true;
        continue;
      }

      auto new_state = std::make_pair(neighbor, new_stack);
      if (visited.find(new_state) == visited.end()) {
        slice.insert(neighbor);
        worklist.push({neighbor, new_stack});
      }
    }
  }

  if (diagnostics != nullptr)
    diagnostics->states_explored = visited.size();
  return slice;
}

ContextSensitiveSlicing::ProcedureSummary
ContextSensitiveSlicing::computeProcedureSummary(
    Node *entry_node, Node *call_site, const std::set<EdgeType> &edge_types,
    bool forward) {
  ProcedureSummary sum;
  VisitedSet visited;
  std::queue<std::pair<Node *, std::vector<Node *>>> worklist;
  std::vector<Node *> stack = {call_site};
  worklist.push({entry_node, stack});
  sum.reachable.insert(entry_node);

  while (!worklist.empty()) {
    Node *current = worklist.front().first;
    std::vector<Node *> call_stack = worklist.front().second;
    worklist.pop();

    auto state = std::make_pair(current, call_stack);
    if (visited.count(state))
      continue;
    visited.insert(state);
    sum.reachable.insert(current);

    auto &edges = forward ? current->getOutEdgeSet() : current->getInEdgeSet();
    for (auto *edge : edges) {
      if (edge == nullptr ||
          (!edge_types.empty() &&
           edge_types.find(edge->getEdgeType()) == edge_types.end())) {
        continue;
      }
      Node *neighbor = forward ? edge->getDstNode() : edge->getSrcNode();
      if (neighbor == nullptr)
        continue;

      std::vector<Node *> new_stack = call_stack;
      if (!applyCallStackTransition(forward, current, neighbor,
                                    edge->getEdgeType(), new_stack)) {
        continue;
      }
      if (call_stack.size() == 1u && new_stack.empty() &&
          (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLINV ||
           edge->getEdgeType() == EdgeType::CONTROLDEP_CALLRET)) {
        sum.returns_to_caller = true;
      }
      auto new_state = std::make_pair(neighbor, new_stack);
      if (visited.find(new_state) == visited.end()) {
        sum.reachable.insert(neighbor);
        worklist.push({neighbor, new_stack});
      }
    }
  }
  return sum;
}

// Note: getAssociatedCallNode is no longer needed in the full CFL-reachability
// implementation The PDG already connects return instructions directly to their
// corresponding call sites via CONTROLDEP_CALLRET edges, so we can use direct
// edge traversal for proper CFL-reachability

// ==================== ContextSensitiveSlicingUtils Implementation
// ====================

std::set<EdgeType> ContextSensitiveSlicingUtils::getCallReturnEdges() {
  return {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
          EdgeType::PARAMETER_IN, EdgeType::PARAMETER_OUT, EdgeType::DATA_RET};
}

std::unordered_map<std::string, size_t>
ContextSensitiveSlicingUtils::compareSlices(const NodeSet &cs_slice,
                                            const NodeSet &ci_slice) {
  std::unordered_map<std::string, size_t> comparison;
  comparison["cs_slice_size"] = cs_slice.size();
  comparison["ci_slice_size"] = ci_slice.size();

  size_t cs_only = 0, ci_only = 0, common = 0;
  for (auto *node : cs_slice) {
    if (ci_slice.find(node) == ci_slice.end()) {
      cs_only++;
    } else {
      common++;
    }
  }
  for (auto *node : ci_slice) {
    if (cs_slice.find(node) == cs_slice.end()) {
      ci_only++;
    }
  }

  comparison["cs_only_nodes"] = cs_only;
  comparison["ci_only_nodes"] = ci_only;
  comparison["common_nodes"] = common;

  if (ci_slice.size() > 0) {
    comparison["precision_improvement_percent"] =
        static_cast<size_t>((double)ci_only / ci_slice.size() * 100.0);
  }

  return comparison;
}

void ContextSensitiveSlicingUtils::printContextSensitiveSlice(
    const NodeSet &slice, const std::string &slice_name) {
  errs() << "=============== Context-Sensitive " << slice_name
         << " ===============\n";
  errs() << "Total nodes: " << slice.size() << "\n";

  for (auto *node : slice) {
    if (node == nullptr)
      continue;
    errs() << "node: " << node << " - "
           << pdgutils::getNodeTypeStr(node->getNodeType()) << "\n";
  }
  errs() << "==========================================\n";
}

std::unordered_map<std::string, size_t>
ContextSensitiveSlicingUtils::getContextSensitiveSliceStatistics(
    const NodeSet &slice) {
  std::unordered_map<std::string, size_t> stats;
  stats["total_nodes"] = slice.size();

  std::unordered_map<GraphNodeType, size_t> node_type_counts;
  std::unordered_map<EdgeType, size_t> edge_type_counts;
  size_t call_nodes = 0, return_nodes = 0, parameter_nodes = 0;

  for (auto *node : slice) {
    if (node == nullptr)
      continue;

    GraphNodeType node_type = node->getNodeType();
    node_type_counts[node_type]++;

    if (node_type == GraphNodeType::INST_FUNCALL) {
      call_nodes++;
    } else if (node_type == GraphNodeType::INST_RET) {
      return_nodes++;
    } else if (node_type == GraphNodeType::PARAM_FORMALIN ||
               node_type == GraphNodeType::PARAM_FORMALOUT ||
               node_type == GraphNodeType::PARAM_ACTUALIN ||
               node_type == GraphNodeType::PARAM_ACTUALOUT) {
      parameter_nodes++;
    }

    for (auto *edge : node->getInEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
    for (auto *edge : node->getOutEdgeSet())
      edge_type_counts[edge->getEdgeType()]++;
  }

  for (const auto &pair : node_type_counts)
    stats["node_type_" + pdgutils::getNodeTypeStr(pair.first)] = pair.second;
  for (const auto &pair : edge_type_counts)
    stats["edge_type_" + pdgutils::getEdgeTypeStr(pair.first)] = pair.second;

  stats["call_nodes"] = call_nodes;
  stats["return_nodes"] = return_nodes;
  stats["parameter_nodes"] = parameter_nodes;

  return stats;
}

std::unordered_map<std::string, size_t>
ContextSensitiveSlicingUtils::getCFLReachabilityStatistics(
    const NodeSet &slice) {
  std::unordered_map<std::string, size_t> stats;
  stats["total_nodes"] = slice.size();

  size_t call_nodes = 0, return_nodes = 0, matched_pairs = 0,
         unmatched_calls = 0, unmatched_returns = 0;

  for (auto *node : slice) {
    if (node == nullptr)
      continue;

    GraphNodeType node_type = node->getNodeType();
    if (node_type == GraphNodeType::INST_FUNCALL) {
      call_nodes++;
      bool has_return_edges = false;
      for (auto *edge : node->getOutEdgeSet()) {
        if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLRET) {
          has_return_edges = true;
          break;
        }
      }
      if (has_return_edges)
        matched_pairs++;
      else
        unmatched_calls++;
    } else if (node_type == GraphNodeType::INST_RET) {
      return_nodes++;
      bool has_call_edges = false;
      for (auto *edge : node->getInEdgeSet()) {
        if (edge->getEdgeType() == EdgeType::CONTROLDEP_CALLINV) {
          has_call_edges = true;
          break;
        }
      }
      if (!has_call_edges)
        unmatched_returns++;
    }
  }

  stats["call_nodes"] = call_nodes;
  stats["return_nodes"] = return_nodes;
  stats["matched_call_return_pairs"] = matched_pairs;
  stats["unmatched_calls"] = unmatched_calls;
  stats["unmatched_returns"] = unmatched_returns;

  if (call_nodes > 0) {
    stats["call_match_percentage"] =
        static_cast<size_t>((double)matched_pairs / call_nodes * 100.0);
  }

  return stats;
}

bool ContextSensitiveSlicingUtils::isCFLValidPath(
    const std::vector<Node *> &path, GenericGraph & /*pdg*/) {
  if (path.empty())
    return true;

  std::vector<Node *> call_stack;

  for (size_t i = 0; i < path.size() - 1; ++i) {
    Node *current = path[i];
    Node *next = path[i + 1];

    if (!current || !next)
      continue;

    EdgeType edge_type = EdgeType::TYPE_OTHEREDGE;
    for (auto *edge : current->getOutEdgeSet()) {
      if (edge && edge->getDstNode() == next) {
        edge_type = edge->getEdgeType();
        break;
      }
    }

    if (edge_type == EdgeType::CONTROLDEP_CALLINV) {
      call_stack.push_back(current);
    } else if (edge_type == EdgeType::CONTROLDEP_CALLRET) {
      // Return edge: current (callee) -> next (call site); stack top must be
      // the call site we return to.
      if (call_stack.empty() || call_stack.back() != next) {
        return false;
      }
      call_stack.pop_back();
    }
  }

  return call_stack.empty();
}

} // namespace pdg
