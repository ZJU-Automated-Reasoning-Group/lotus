#include "IR/PDG/Analysis/TransformQuery.h"

#include "IR/PDG/Analysis/Internal/QuerySupport.h"
#include "IR/PDG/Analysis/Query.h"

#include <functional>
#include <queue>
#include <unordered_set>

namespace pdg {
using namespace llvm;
using namespace query_detail;

static bool nodesAreInSameFunction(Node &lhs, Node &rhs,
                                   const LLVMQueryContext &llvm_context) {
  if (lhs.getFunc() == nullptr || rhs.getFunc() == nullptr)
    return llvm_context.function == nullptr || lhs.getFunc() == rhs.getFunc();
  return lhs.getFunc() == rhs.getFunc();
}

static bool instructionBlocked(const Instruction &inst,
                               const LLVMQueryContext &llvm_context,
                               std::string &reason) {
  if (isa<PHINode>(&inst)) {
    reason = "PHI nodes are not movable";
    return true;
  }
  if (inst.isTerminator()) {
    reason = "Terminator instructions are not movable";
    return true;
  }
  if (isa<AllocaInst>(&inst)) {
    reason = "Alloca instructions are not movable";
    return true;
  }
  if ((inst.mayReadOrWriteMemory() || inst.mayHaveSideEffects()) &&
      llvm_context.memory_ssa == nullptr) {
    reason = "MemorySSA is required for memory-affecting motion";
    return true;
  }
  if (inst.mayThrow()) {
    reason = "Potentially throwing instructions are not movable";
    return true;
  }
  return false;
}

MotionCheckResult
TransformQuery::canMoveEarlier(Node &moving_node, Node &anchor_node,
                               const LLVMQueryContext &llvm_context,
                               const PDGQueryOptions &options) const {
  MotionCheckResult result;
  result.moving_node = &moving_node;
  result.anchor_node = &anchor_node;

  if (&moving_node == &anchor_node) {
    result.legal = true;
    result.reason = "Trivial move";
    return result;
  }

  if (!nodesAreInSameFunction(moving_node, anchor_node, llvm_context)) {
    result.reason = "Node motion across functions is disallowed";
    return result;
  }

  const Instruction *moving_inst =
      dyn_cast_or_null<Instruction>(moving_node.getValue());
  const Instruction *anchor_inst =
      dyn_cast_or_null<Instruction>(anchor_node.getValue());
  if (moving_inst != nullptr &&
      instructionBlocked(*moving_inst, llvm_context, result.reason))
    return result;

  if (llvm_context.dominator_tree != nullptr && moving_inst != nullptr &&
      anchor_inst != nullptr &&
      !llvm_context.dominator_tree->dominates(anchor_inst, moving_inst)) {
    result.reason = "Anchor does not dominate moving instruction";
    return result;
  }

  PDGQueryOptions query_options = options;
  query_options.edge_preset = PDGEdgePreset::TransformLegality;
  DependenceQuery query(pdg_);
  PDGCriteria source_criteria;
  PDGCriteria target_criteria;
  source_criteria.nodes.insert(&anchor_node);
  target_criteria.nodes.insert(&moving_node);
  PDGQueryResult path = query.shortestPath(source_criteria, target_criteria,
                                           query_options, nullptr);
  if (!path.witness_paths.empty()) {
    result.reason = "Anchor transitively constrains moving node";
    result.blocking_path = path.witness_paths.front().nodes;
    result.blocking_edge_types = path.witness_paths.front().edge_types;
    return result;
  }

  result.legal = true;
  result.reason = "No blocking dependence found";
  if (llvm_context.memory_ssa == nullptr)
    result.diagnostics.notes.push_back(
        "Performed conservative motion check without MemorySSA");
  return result;
}

MotionCheckResult
TransformQuery::canMoveLater(Node &moving_node, Node &anchor_node,
                             const LLVMQueryContext &llvm_context,
                             const PDGQueryOptions &options) const {
  MotionCheckResult result;
  result.moving_node = &moving_node;
  result.anchor_node = &anchor_node;

  if (&moving_node == &anchor_node) {
    result.legal = true;
    result.reason = "Trivial move";
    return result;
  }

  if (!nodesAreInSameFunction(moving_node, anchor_node, llvm_context)) {
    result.reason = "Node motion across functions is disallowed";
    return result;
  }

  const Instruction *moving_inst =
      dyn_cast_or_null<Instruction>(moving_node.getValue());
  const Instruction *anchor_inst =
      dyn_cast_or_null<Instruction>(anchor_node.getValue());
  if (moving_inst != nullptr &&
      instructionBlocked(*moving_inst, llvm_context, result.reason))
    return result;

  if (llvm_context.post_dominator_tree != nullptr && moving_inst != nullptr &&
      anchor_inst != nullptr &&
      !llvm_context.post_dominator_tree->dominates(anchor_inst->getParent(),
                                                   moving_inst->getParent())) {
    result.reason = "Anchor does not post-dominate moving instruction";
    return result;
  }

  PDGQueryOptions query_options = options;
  query_options.edge_preset = PDGEdgePreset::TransformLegality;
  DependenceQuery query(pdg_);
  PDGCriteria source_criteria;
  PDGCriteria target_criteria;
  source_criteria.nodes.insert(&moving_node);
  target_criteria.nodes.insert(&anchor_node);
  PDGQueryResult path = query.shortestPath(source_criteria, target_criteria,
                                           query_options, nullptr);
  if (!path.witness_paths.empty()) {
    result.reason = "Moving node transitively constrains anchor";
    result.blocking_path = path.witness_paths.front().nodes;
    result.blocking_edge_types = path.witness_paths.front().edge_types;
    return result;
  }

  result.legal = true;
  result.reason = "No blocking dependence found";
  if (llvm_context.memory_ssa == nullptr)
    result.diagnostics.notes.push_back(
        "Performed conservative motion check without MemorySSA");
  return result;
}

IndependenceCheckResult
TransformQuery::independent(Node &a, Node &b,
                            const LLVMQueryContext &llvm_context,
                            const PDGQueryOptions &options) const {
  (void)llvm_context;
  IndependenceCheckResult result;
  PDGQueryOptions local_options = options;
  local_options.edge_preset = PDGEdgePreset::TransformLegality;
  DependenceQuery query(pdg_);

  PDGCriteria a_criteria;
  PDGCriteria b_criteria;
  a_criteria.nodes.insert(&a);
  b_criteria.nodes.insert(&b);

  PDGQueryResult ab =
      query.shortestPath(a_criteria, b_criteria, local_options, nullptr);
  PDGQueryResult ba =
      query.shortestPath(b_criteria, a_criteria, local_options, nullptr);
  if (!ab.witness_paths.empty()) {
    result.witness_path_ab = ab.witness_paths.front().nodes;
    result.witness_edge_types_ab = ab.witness_paths.front().edge_types;
  }
  if (!ba.witness_paths.empty()) {
    result.witness_path_ba = ba.witness_paths.front().nodes;
    result.witness_edge_types_ba = ba.witness_paths.front().edge_types;
  }
  result.independent =
      result.witness_path_ab.empty() && result.witness_path_ba.empty();
  return result;
}

PDGQueryResult TransformQuery::readySet(const PDGQueryScope &scope,
                                        const NodeSet &scheduled,
                                        const LLVMQueryContext &llvm_context,
                                        const PDGQueryOptions &options) const {
  (void)llvm_context;
  PDGQueryResult result;
  const NodeSet region = scopeNodes(pdg_, scope);
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::TransformLegality);

  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    Node *node = *it;
    if (node == nullptr || scheduled.count(node) != 0)
      continue;
    bool ready = true;
    for (Node::EdgeSet::const_iterator edge_it = node->getInEdgeSet().begin();
         edge_it != node->getInEdgeSet().end(); ++edge_it) {
      Edge *edge = *edge_it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *pred = edge->getSrcNode();
      if (pred != nullptr && region.count(pred) != 0 &&
          scheduled.count(pred) == 0) {
        ready = false;
        break;
      }
    }
    if (ready)
      result.nodes.insert(node);
  }

  result.edges = collectInducedEdges(result.nodes,
                                     edgeTypesForPreset(options.edge_preset));
  return result;
}

std::vector<NodeSet> TransformQuery::stronglyConnectedComponents(
    const PDGQueryScope &scope, const LLVMQueryContext &llvm_context,
    const PDGQueryOptions &options) const {
  (void)llvm_context;
  const NodeSet region = scopeNodes(pdg_, scope);
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::TransformLegality);
  std::unordered_map<Node *, int> index;
  std::unordered_map<Node *, int> lowlink;
  std::unordered_set<Node *> on_stack;
  std::vector<Node *> stack;
  std::vector<NodeSet> components;
  int next_index = 0;

  std::function<void(Node *)> visit = [&](Node *node) {
    index[node] = next_index;
    lowlink[node] = next_index;
    next_index++;
    stack.push_back(node);
    on_stack.insert(node);

    for (Node::EdgeSet::const_iterator it = node->getOutEdgeSet().begin();
         it != node->getOutEdgeSet().end(); ++it) {
      Edge *edge = *it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *succ = edge->getDstNode();
      if (succ == nullptr || region.count(succ) == 0)
        continue;
      if (index.count(succ) == 0) {
        visit(succ);
        lowlink[node] = std::min(lowlink[node], lowlink[succ]);
      } else if (on_stack.count(succ) != 0) {
        lowlink[node] = std::min(lowlink[node], index[succ]);
      }
    }

    if (lowlink[node] == index[node]) {
      NodeSet component;
      while (!stack.empty()) {
        Node *member = stack.back();
        stack.pop_back();
        on_stack.erase(member);
        component.insert(member);
        if (member == node)
          break;
      }
      components.push_back(component);
    }
  };

  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    if (index.count(*it) == 0)
      visit(*it);
  }
  return components;
}

std::vector<NodeSet>
TransformQuery::topologicalLevels(const PDGQueryScope &scope,
                                  const LLVMQueryContext &llvm_context,
                                  const PDGQueryOptions &options) const {
  (void)options;
  std::vector<NodeSet> levels;
  const NodeSet region = scopeNodes(pdg_, scope);
  if (region.empty())
    return levels;

  std::vector<NodeSet> components =
      stronglyConnectedComponents(scope, llvm_context, options);
  std::unordered_map<Node *, size_t> node_to_component;
  for (size_t index = 0; index < components.size(); ++index) {
    for (NodeSet::const_iterator it = components[index].begin();
         it != components[index].end(); ++it)
      node_to_component[*it] = index;
  }

  std::vector<std::set<size_t>> adjacency(components.size());
  std::vector<size_t> indegree(components.size(), 0);
  const std::set<EdgeType> edge_types =
      edgeTypesForPreset(PDGEdgePreset::TransformLegality);
  for (NodeSet::const_iterator it = region.begin(); it != region.end(); ++it) {
    Node *node = *it;
    for (Node::EdgeSet::const_iterator edge_it = node->getOutEdgeSet().begin();
         edge_it != node->getOutEdgeSet().end(); ++edge_it) {
      Edge *edge = *edge_it;
      if (edge == nullptr || !isEdgeAllowed(edge->getEdgeType(), edge_types))
        continue;
      Node *succ = edge->getDstNode();
      if (succ == nullptr || region.count(succ) == 0)
        continue;
      size_t from = node_to_component[node];
      size_t to = node_to_component[succ];
      if (from != to && adjacency[from].insert(to).second)
        indegree[to]++;
    }
  }

  std::queue<size_t> ready;
  for (size_t i = 0; i < indegree.size(); ++i) {
    if (indegree[i] == 0)
      ready.push(i);
  }

  while (!ready.empty()) {
    size_t layer_size = ready.size();
    NodeSet level;
    for (size_t i = 0; i < layer_size; ++i) {
      size_t component = ready.front();
      ready.pop();
      level.insert(components[component].begin(), components[component].end());
      for (std::set<size_t>::const_iterator it = adjacency[component].begin();
           it != adjacency[component].end(); ++it) {
        if (--indegree[*it] == 0)
          ready.push(*it);
      }
    }
    levels.push_back(level);
  }

  return levels;
}

size_t
TransformQuery::criticalPathLength(const PDGQueryScope &scope,
                                   const LLVMQueryContext &llvm_context,
                                   const PDGQueryOptions &options) const {
  std::vector<NodeSet> levels = topologicalLevels(scope, llvm_context, options);
  size_t count = 0;
  for (size_t i = 0; i < levels.size(); ++i)
    count += levels[i].empty() ? 0 : 1;
  return count == 0 ? 0 : count - 1;
}

} // namespace pdg
