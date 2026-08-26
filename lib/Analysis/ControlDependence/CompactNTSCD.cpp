//===- CompactNTSCD.cpp - Inevitability-based NTSCD ----------------------===//

#include "Analysis/ControlDependence/CompactControlDependence.h"

#include <cassert>
#include <deque>

namespace lotus::cd::detail {

bool Inevitability::contains(const GraphNode *source,
                             const GraphNode *target) const {
  if (!source || !target || source->getID() == 0 ||
      source->getID() > m_rows.size())
    return false;
  return m_rows[source->getID() - 1].test(target->getID());
}

const llvm::SparseBitVector<> &
Inevitability::row(const GraphNode *source) const {
  assert(source && source->getID() > 0 && source->getID() <= m_rows.size());
  return m_rows[source->getID() - 1];
}

Inevitability computeInevitability(Graph &graph) {
  Inevitability result(graph.size());
  std::vector<bool> marked(graph.size());
  std::vector<size_t> remaining(graph.size());
  std::deque<GraphNode *> worklist;

  for (GraphNode *target : graph.nodes()) {
    std::fill(marked.begin(), marked.end(), false);
    for (GraphNode *node : graph.nodes())
      remaining[node->getID() - 1] = node->successors().size();
    worklist.clear();

    marked[target->getID() - 1] = true;
    result.m_rows[target->getID() - 1].set(target->getID());
    worklist.push_back(target);

    while (!worklist.empty()) {
      GraphNode *current = worklist.front();
      worklist.pop_front();
      for (GraphNode *predecessor : current->predecessors()) {
        const size_t index = predecessor->getID() - 1;
        // This guard is part of the counter invariant: a marked source must
        // never be counted or propagated a second time.
        if (marked[index])
          continue;
        assert(remaining[index] > 0);
        --remaining[index];
        if (remaining[index] == 0 && !predecessor->successors().empty()) {
          marked[index] = true;
          result.m_rows[index].set(target->getID());
          worklist.push_back(predecessor);
        }
      }
    }
  }
  return result;
}

DependenceResult computeCompactNTSCD(Graph &graph,
                                     const Inevitability &inevitability) {
  assert(inevitability.size() == graph.size());
  DependenceMap dependencies;
  DependenceMap dependents;

  for (GraphNode *decision : graph.predicates()) {
    const size_t successorCount = decision->successors().size();
    if (successorCount < 2)
      continue;
    for (GraphNode *target : graph.nodes()) {
      size_t inevitableSuccessors = 0;
      for (GraphNode *successor : decision->successors())
        inevitableSuccessors += inevitability.contains(successor, target);
      if (inevitableSuccessors > 0 && inevitableSuccessors < successorCount) {
        dependencies[target].insert(decision);
        dependents[decision].insert(target);
      }
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

} // namespace lotus::cd::detail
