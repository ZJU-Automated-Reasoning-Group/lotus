/**
 * Generic graph slice and prune utilities (borrowed from WALA's GraphSlicer).
 *
 * - slice_backward: nodes backward-reachable from roots (following predecessor
 * edges).
 * - slice_forward: nodes forward-reachable from roots (following successor
 * edges).
 * - prune_nodes: subset of nodes satisfying a predicate (e.g. for subgraph
 * views).
 */

#pragma once

#include <queue>
#include <unordered_set>
#include <vector>

namespace lotus {

/**
 * Backward slice: from \p roots, follow \p get_predecessors until fixpoint.
 * \p get_predecessors(n) must return an iterable of nodes (e.g.
 * std::vector<Node>).
 */
template <typename Node, typename GetPred>
std::unordered_set<Node> slice_backward(const std::vector<Node> &roots,
                                        GetPred get_predecessors) {
  std::unordered_set<Node> result;
  std::queue<Node> worklist;
  for (Node n : roots) {
    if (result.insert(n).second)
      worklist.push(n);
  }
  while (!worklist.empty()) {
    Node n = worklist.front();
    worklist.pop();
    for (Node pred : get_predecessors(n)) {
      if (result.insert(pred).second)
        worklist.push(pred);
    }
  }
  return result;
}

/**
 * Backward slice from a single root.
 */
template <typename Node, typename GetPred>
std::unordered_set<Node> slice_backward(Node root, GetPred get_predecessors) {
  return slice_backward(std::vector<Node>{root}, get_predecessors);
}

/**
 * Forward slice: from \p roots, follow \p get_successors until fixpoint.
 */
template <typename Node, typename GetSucc>
std::unordered_set<Node> slice_forward(const std::vector<Node> &roots,
                                       GetSucc get_successors) {
  std::unordered_set<Node> result;
  std::queue<Node> worklist;
  for (Node n : roots) {
    if (result.insert(n).second)
      worklist.push(n);
  }
  while (!worklist.empty()) {
    Node n = worklist.front();
    worklist.pop();
    for (Node succ : get_successors(n)) {
      if (result.insert(succ).second)
        worklist.push(succ);
    }
  }
  return result;
}

/**
 * Forward slice from a single root.
 */
template <typename Node, typename GetSucc>
std::unordered_set<Node> slice_forward(Node root, GetSucc get_successors) {
  return slice_forward(std::vector<Node>{root}, get_successors);
}

/**
 * Prune: return the set of nodes in \p nodes that satisfy \p pred.
 * Use with slice results to restrict to a subgraph (e.g. only certain kinds of
 * nodes).
 */
template <typename Node, typename Predicate>
std::unordered_set<Node> prune_nodes(const std::unordered_set<Node> &nodes,
                                     Predicate pred) {
  std::unordered_set<Node> result;
  for (Node n : nodes) {
    if (pred(n))
      result.insert(n);
  }
  return result;
}

/**
 * Prune (iterator version): return nodes from \p begin..\p end that satisfy \p
 * pred.
 */
template <typename Iterator, typename Node, typename Predicate>
std::unordered_set<Node> prune_nodes(Iterator begin, Iterator end,
                                     Predicate pred) {
  std::unordered_set<Node> result;
  for (; begin != end; ++begin) {
    Node n = *begin;
    if (pred(n))
      result.insert(n);
  }
  return result;
}

} // namespace lotus
