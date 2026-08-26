//===- NTSCD.cpp - Non-termination-sensitive control dependence ----------===//
//
// Adapted from dg's generic NTSCD algorithms (MIT license).
//
//===----------------------------------------------------------------------===//

#include "Analysis/ControlDependence/NTSCD.h"

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
  T popFront() {
    T value = m_queue.front();
    m_queue.pop_front();
    return value;
  }

private:
  std::deque<T> m_queue;
  std::unordered_set<T> m_present;
};

} // namespace

DependenceResult computeNTSCD(Graph &graph) {
  DependenceMap dependencies;
  DependenceMap dependents;
  std::unordered_map<GraphNode *, unsigned> colors;

  for (GraphNode *target : graph.nodes()) {
    const unsigned color = target->getID();
    colors[target] = color;
    NodeSet frontier;
    for (GraphNode *pred : target->predecessors())
      if (colors[pred] != color)
        frontier.insert(pred);

    bool progress;
    do {
      progress = false;
      NodeSet next;
      for (GraphNode *node : frontier) {
        bool allColored = !node->successors().empty();
        for (GraphNode *succ : node->successors())
          allColored &= colors[succ] == color;
        if (allColored) {
          colors[node] = color;
          for (GraphNode *pred : node->predecessors())
            if (colors[pred] != color)
              next.insert(pred);
          progress = true;
        } else {
          next.insert(node);
        }
      }
      frontier.swap(next);
    } while (progress);

    for (GraphNode *predicate : frontier) {
      if (!graph.isPredicate(*predicate))
        continue;
      bool hasColored = false;
      bool hasUncolored = false;
      for (GraphNode *succ : predicate->successors()) {
        hasColored |= colors[succ] == color;
        hasUncolored |= colors[succ] != color;
      }
      if (hasColored && hasUncolored) {
        dependencies[target].insert(predicate);
        dependents[predicate].insert(target);
      }
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

DependenceResult computeNTSCD2(Graph &graph) {
  DependenceMap dependencies;
  DependenceMap dependents;
  std::unordered_map<GraphNode *, bool> colored;
  std::unordered_map<GraphNode *, size_t> counters;

  for (GraphNode *target : graph.nodes()) {
    for (GraphNode *node : graph.nodes()) {
      colored[node] = false;
      counters[node] = node->successors().size();
    }
    colored[target] = true;
    std::vector<GraphNode *> worklist{target};
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

    for (GraphNode *predicate : graph.predicates()) {
      bool hasColored = false;
      bool hasUncolored = false;
      for (GraphNode *succ : predicate->successors()) {
        hasColored |= colored[succ];
        hasUncolored |= !colored[succ];
      }
      if (hasColored && hasUncolored) {
        dependencies[target].insert(predicate);
        dependents[predicate].insert(target);
      }
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

DependenceResult computeNTSCDRanganath(Graph &graph, bool fixed) {
  using Symbol = std::pair<GraphNode *, GraphNode *>;
  using Symbols = std::set<Symbol>;
  std::unordered_map<GraphNode *, std::unordered_map<GraphNode *, Symbols>>
      symbols;
  SetQueue<GraphNode *> workbag;

  auto processNode = [&](GraphNode *node) {
    bool changed = false;
    if (node->successors().size() == 1 && node->successors().front() != node) {
      GraphNode *successor = node->successors().front();
      for (GraphNode *predicate : graph.predicates()) {
        for (const Symbol &symbol : symbols[node][predicate]) {
          if (symbols[successor][predicate].insert(symbol).second) {
            changed = true;
            workbag.push(successor);
          }
        }
      }
    } else if (node->successors().size() > 1) {
      for (GraphNode *candidate : graph.nodes()) {
        if (symbols[candidate][node].size() != node->successors().size())
          continue;
        for (GraphNode *predicate : graph.predicates()) {
          if (predicate == node)
            continue;
          for (const Symbol &symbol : symbols[node][predicate]) {
            if (symbols[candidate][predicate].insert(symbol).second) {
              changed = true;
              workbag.push(candidate);
            }
          }
        }
      }
    }
    return changed;
  };

  for (GraphNode *predicate : graph.predicates()) {
    for (GraphNode *succ : predicate->successors()) {
      symbols[succ][predicate].insert({predicate, succ});
      workbag.push(succ);
    }
  }

  if (fixed) {
    bool changed;
    do {
      changed = false;
      for (GraphNode *node : graph.nodes())
        changed |= processNode(node);
    } while (changed);
  } else {
    while (!workbag.empty())
      processNode(workbag.popFront());
  }

  DependenceMap dependencies;
  DependenceMap dependents;
  for (GraphNode *node : graph.nodes()) {
    for (GraphNode *predicate : graph.predicates()) {
      const Symbols &set = symbols[node][predicate];
      if (!set.empty() && set.size() < predicate->successors().size()) {
        dependencies[node].insert(predicate);
        dependents[predicate].insert(node);
      }
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

} // namespace lotus::cd::detail
