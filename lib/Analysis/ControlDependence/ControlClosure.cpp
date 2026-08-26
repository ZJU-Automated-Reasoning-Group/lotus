//===- ControlClosure.cpp - Strong control closure ------------------------===//
//
// Adapted from dg's strong-control-closure algorithm (MIT license).
//
//===----------------------------------------------------------------------===//

#include "Analysis/ControlDependence/ControlClosure.h"

#include <cassert>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cd::detail {
namespace {

template <typename T> class SetQueue {
public:
  void push(T value) {
    if (m_present.insert(value).second)
      m_queue.push_back(value);
  }
  bool empty() const { return m_queue.empty(); }
  T popBack() {
    T value = m_queue.back();
    m_queue.pop_back();
    return value;
  }

private:
  std::deque<T> m_queue;
  std::unordered_set<T> m_present;
};

} // namespace

NodeSet computeStrongControlClosure(Graph &graph, const NodeSet &nodes) {
  auto firstReachable = [](const NodeSet &selected, GraphNode *from) {
    NodeSet result;
    SetQueue<GraphNode *> queue;
    for (GraphNode *succ : from->successors())
      queue.push(succ);
    while (!queue.empty()) {
      GraphNode *current = queue.popBack();
      if (selected.count(current)) {
        result.insert(current);
      } else {
        for (GraphNode *succ : current->successors())
          queue.push(succ);
      }
    }
    return result;
  };

  auto theta = [&](const NodeSet &selected, GraphNode *node) {
    if (selected.count(node))
      return NodeSet{node};
    return firstReachable(selected, node);
  };

  auto gamma = [&](const NodeSet &targets) {
    std::unordered_map<GraphNode *, bool> colored;
    std::unordered_map<GraphNode *, size_t> counters;
    std::vector<GraphNode *> worklist;
    for (GraphNode *node : graph.nodes()) {
      colored[node] = false;
      counters[node] = node->successors().size();
    }
    for (GraphNode *target : targets) {
      colored[target] = true;
      worklist.push_back(target);
    }
    while (!worklist.empty()) {
      GraphNode *node = worklist.back();
      worklist.pop_back();
      for (GraphNode *pred : node->predecessors()) {
        size_t &counter = counters[pred];
        assert(counter > 0);
        --counter;
        if (counter == 0 && !colored[pred]) {
          colored[pred] = true;
          worklist.push_back(pred);
        }
      }
    }
    NodeSet result;
    for (GraphNode *node : graph.nodes())
      if (!colored[node])
        result.insert(node);
    return result;
  };

  NodeSet closure = nodes;
  while (true) {
    SetQueue<GraphNode *> queue;
    for (GraphNode *node : closure)
      for (GraphNode *succ : node->successors())
        queue.push(succ);

    GraphNode *toAdd = nullptr;
    while (!queue.empty() && !toAdd) {
      GraphNode *predicate = queue.popBack();
      for (GraphNode *succ : predicate->successors()) {
        if (theta(closure, succ).size() != 1)
          continue;
        NodeSet gammaSet = gamma(closure);
        if (gammaSet.count(succ))
          continue;
        if (theta(closure, predicate).size() < 2 && !gammaSet.count(predicate))
          continue;
        toAdd = predicate;
        break;
      }
      if (!toAdd)
        for (GraphNode *succ : predicate->successors())
          queue.push(succ);
    }

    if (!toAdd || !closure.insert(toAdd).second)
      break;
  }
  return closure;
}

} // namespace lotus::cd::detail
