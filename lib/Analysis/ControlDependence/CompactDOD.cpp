//===- CompactDOD.cpp - SCC-based canonical DOD bicliques ----------------===//

#include "Analysis/ControlDependence/CompactControlDependence.h"

#include <algorithm>
#include <cassert>
#include <deque>
#include <optional>
#include <utility>

namespace lotus::cd::detail {
namespace {

constexpr int EMPTY = -1;
constexpr int MANY = -2;

int joinCapped(int left, int right) {
  if (left == MANY || right == MANY)
    return MANY;
  if (left == EMPTY)
    return right;
  if (right == EMPTY)
    return left;
  return left == right ? left : MANY;
}

bool inSet(const llvm::SparseBitVector<> &set, const GraphNode *node) {
  return set.test(node->getID());
}

llvm::SparseBitVector<> firstHits(Graph &graph, GraphNode *start,
                                  const llvm::SparseBitVector<> &set) {
  llvm::SparseBitVector<> hits;
  if (inSet(set, start)) {
    hits.set(start->getID());
    return hits;
  }

  std::vector<bool> seen(graph.size() + 1, false);
  std::vector<GraphNode *> worklist{start};
  seen[start->getID()] = true;
  while (!worklist.empty()) {
    GraphNode *current = worklist.back();
    worklist.pop_back();
    for (GraphNode *successor : current->successors()) {
      if (inSet(set, successor)) {
        hits.set(successor->getID());
      } else if (!seen[successor->getID()]) {
        seen[successor->getID()] = true;
        worklist.push_back(successor);
      }
    }
  }
  return hits;
}

struct OutsideSCCInfo {
  std::vector<int> component;
  std::vector<int> labels;
};

OutsideSCCInfo computeOutsideSCCs(Graph &graph,
                                  const llvm::SparseBitVector<> &set) {
  const size_t nodeCount = graph.size();
  std::vector<bool> seen(nodeCount + 1, false);
  std::vector<GraphNode *> finish;
  finish.reserve(nodeCount);

  // Iterative first Kosaraju pass on G[V-S].
  for (GraphNode *root : graph.nodes()) {
    if (inSet(set, root) || seen[root->getID()])
      continue;
    seen[root->getID()] = true;
    std::vector<std::pair<GraphNode *, size_t>> stack{{root, 0}};
    while (!stack.empty()) {
      GraphNode *node = stack.back().first;
      size_t &nextIndex = stack.back().second;
      const auto &successors = node->successors();
      while (nextIndex < successors.size() && inSet(set, successors[nextIndex]))
        ++nextIndex;
      if (nextIndex == successors.size()) {
        finish.push_back(node);
        stack.pop_back();
        continue;
      }
      GraphNode *successor = successors[nextIndex++];
      if (!seen[successor->getID()]) {
        seen[successor->getID()] = true;
        stack.push_back({successor, 0});
      }
    }
  }

  std::vector<int> component(nodeCount + 1, -1);
  int componentCount = 0;
  for (auto it = finish.rbegin(); it != finish.rend(); ++it) {
    GraphNode *root = *it;
    if (component[root->getID()] != -1)
      continue;
    component[root->getID()] = componentCount;
    std::vector<GraphNode *> worklist{root};
    while (!worklist.empty()) {
      GraphNode *node = worklist.back();
      worklist.pop_back();
      for (GraphNode *predecessor : node->predecessors()) {
        if (inSet(set, predecessor) || component[predecessor->getID()] != -1)
          continue;
        component[predecessor->getID()] = componentCount;
        worklist.push_back(predecessor);
      }
    }
    ++componentCount;
  }

  std::vector<std::vector<unsigned>> dag(componentCount);
  std::vector<int> labels(componentCount, EMPTY);
  for (GraphNode *node : graph.nodes()) {
    if (inSet(set, node))
      continue;
    const int sourceComponent = component[node->getID()];
    assert(sourceComponent >= 0);
    for (GraphNode *successor : node->successors()) {
      if (inSet(set, successor)) {
        labels[sourceComponent] =
            joinCapped(labels[sourceComponent], successor->getID());
      } else {
        const int targetComponent = component[successor->getID()];
        assert(targetComponent >= 0);
        if (sourceComponent != targetComponent)
          dag[sourceComponent].push_back(targetComponent);
      }
    }
  }

  std::vector<size_t> indegree(componentCount, 0);
  for (const auto &successors : dag)
    for (unsigned target : successors)
      ++indegree[target];
  std::deque<unsigned> queue;
  for (unsigned componentID = 0; componentID < componentCount; ++componentID)
    if (indegree[componentID] == 0)
      queue.push_back(componentID);
  std::vector<unsigned> topologicalOrder;
  topologicalOrder.reserve(componentCount);
  while (!queue.empty()) {
    const unsigned componentID = queue.front();
    queue.pop_front();
    topologicalOrder.push_back(componentID);
    for (unsigned target : dag[componentID])
      if (--indegree[target] == 0)
        queue.push_back(target);
  }
  assert(topologicalOrder.size() == static_cast<size_t>(componentCount) &&
         "SCC condensation must be acyclic");
  for (auto it = topologicalOrder.rbegin(); it != topologicalOrder.rend(); ++it)
    for (unsigned target : dag[*it])
      labels[*it] = joinCapped(labels[*it], labels[target]);

  return {std::move(component), std::move(labels)};
}

llvm::SparseBitVector<> cyclicInterval(llvm::ArrayRef<GraphNode *> cycle,
                                       size_t begin, size_t end) {
  llvm::SparseBitVector<> result;
  for (size_t index = begin; index != end; index = (index + 1) % cycle.size())
    result.set(cycle[index]->getID());
  return result;
}

std::optional<DODBiclique>
computeBicliqueFor(Graph &graph, GraphNode *decision,
                   const Inevitability &inevitability) {
  if (decision->successors().size() != 2)
    return std::nullopt;
  const auto &set = inevitability.row(decision);
  if (!set.test(decision->getID()))
    return std::nullopt;

  GraphNode *firstSuccessor = decision->successors()[0];
  GraphNode *secondSuccessor = decision->successors()[1];
  llvm::SparseBitVector<> firstEntries = firstHits(graph, firstSuccessor, set);
  llvm::SparseBitVector<> secondEntries =
      firstHits(graph, secondSuccessor, set);
  llvm::SparseBitVector<> allEntries = firstEntries;
  allEntries |= secondEntries;
  if (allEntries.count() <= 1 || firstEntries.intersects(secondEntries) ||
      allEntries.test(decision->getID()))
    return std::nullopt;

  OutsideSCCInfo outside = computeOutsideSCCs(graph, set);
  std::vector<int> successorByID(graph.size() + 1, EMPTY);
  for (GraphNode *node : graph.nodes()) {
    if (!set.test(node->getID()) || node == decision)
      continue;
    int projectionSuccessor = EMPTY;
    for (GraphNode *successor : node->successors()) {
      int value = successor->getID();
      if (!set.test(successor->getID())) {
        const int component = outside.component[successor->getID()];
        if (component < 0)
          return std::nullopt;
        value = outside.labels[component];
      }
      projectionSuccessor = joinCapped(projectionSuccessor, value);
    }
    if (projectionSuccessor <= 0 ||
        projectionSuccessor == static_cast<int>(decision->getID()))
      return std::nullopt;
    successorByID[node->getID()] = projectionSuccessor;
  }

  const int firstCycleID = [&]() {
    for (unsigned id : set)
      if (id != decision->getID())
        return static_cast<int>(id);
    return EMPTY;
  }();
  if (firstCycleID <= 0)
    return std::nullopt;

  std::vector<GraphNode *> cycle;
  llvm::SparseBitVector<> seen;
  int currentID = firstCycleID;
  while (currentID > 0 && !seen.test(currentID)) {
    if (currentID >= static_cast<int>(successorByID.size()) ||
        successorByID[currentID] <= 0)
      return std::nullopt;
    seen.set(currentID);
    cycle.push_back(graph.getNode(currentID));
    currentID = successorByID[currentID];
  }
  if (currentID != firstCycleID || seen.count() + 1 != set.count())
    return std::nullopt;
  for (unsigned id : set)
    if (id != decision->getID() && !seen.test(id))
      return std::nullopt;

  struct MarkedEntry {
    size_t index;
    unsigned label;
  };
  std::vector<MarkedEntry> marked;
  for (size_t index = 0; index < cycle.size(); ++index) {
    const unsigned id = cycle[index]->getID();
    if (firstEntries.test(id))
      marked.push_back({index, 1});
    else if (secondEntries.test(id))
      marked.push_back({index, 2});
  }
  if (marked.empty())
    return std::nullopt;

  struct Transition {
    MarkedEntry from;
    MarkedEntry to;
  };
  std::vector<Transition> transitions;
  for (size_t index = 0; index < marked.size(); ++index) {
    const MarkedEntry &current = marked[index];
    const MarkedEntry &next = marked[(index + 1) % marked.size()];
    if (current.label != next.label)
      transitions.push_back({current, next});
  }
  if (transitions.size() != 2)
    return std::nullopt;

  std::optional<size_t> alpha;
  std::optional<size_t> beta;
  std::optional<size_t> gamma;
  std::optional<size_t> delta;
  for (const Transition &transition : transitions) {
    if (transition.from.label == 1 && transition.to.label == 2) {
      alpha = transition.from.index;
      beta = transition.to.index;
    } else if (transition.from.label == 2 && transition.to.label == 1) {
      gamma = transition.from.index;
      delta = transition.to.index;
    }
  }
  if (!alpha || !beta || !gamma || !delta)
    return std::nullopt;

  llvm::SparseBitVector<> left = cyclicInterval(cycle, *alpha, *beta);
  llvm::SparseBitVector<> right = cyclicInterval(cycle, *gamma, *delta);
  if (left.empty() || right.empty() || left.intersects(right))
    return std::nullopt;

  return DODBiclique{
      decision,         std::move(left),         std::move(right),
      std::move(cycle), std::move(firstEntries), std::move(secondEntries)};
}

} // namespace

bool DODBiclique::contains(const GraphNode *first,
                           const GraphNode *second) const {
  if (!first || !second || first == second)
    return false;
  return (left.test(first->getID()) && right.test(second->getID())) ||
         (right.test(first->getID()) && left.test(second->getID()));
}

size_t DODBiclique::pairCount() const {
  return static_cast<size_t>(left.count()) * right.count();
}

DODBicliqueMap computeCompactDOD(Graph &graph,
                                 const Inevitability &inevitability) {
  assert(inevitability.size() == graph.size());
  DODBicliqueMap result;
  for (GraphNode *decision : graph.predicates()) {
    if (decision->successors().size() != 2)
      continue;
    std::optional<DODBiclique> biclique =
        computeBicliqueFor(graph, decision, inevitability);
    if (biclique)
      result.emplace(decision, std::move(*biclique));
  }
  return result;
}

DependenceResult
materializeCompactDODDependencies(Graph &graph,
                                  const DODBicliqueMap &bicliques) {
  DependenceMap dependencies;
  DependenceMap dependents;
  for (const auto &entry : bicliques) {
    GraphNode *decision = entry.first;
    llvm::SparseBitVector<> endpoints = entry.second.left;
    endpoints |= entry.second.right;
    for (unsigned id : endpoints) {
      GraphNode *dependent = graph.getNode(id);
      dependencies[dependent].insert(decision);
      dependents[decision].insert(dependent);
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

void forEachDODPair(const Graph &graph, const DODBicliqueMap &bicliques,
                    const std::function<void(GraphNode *, GraphNode *,
                                             GraphNode *)> &callback) {
  for (const auto &entry : bicliques) {
    for (unsigned leftID : entry.second.left) {
      for (unsigned rightID : entry.second.right) {
        GraphNode *left = const_cast<GraphNode *>(graph.getNode(leftID));
        GraphNode *right = const_cast<GraphNode *>(graph.getNode(rightID));
        if (left->getID() > right->getID())
          std::swap(left, right);
        callback(entry.first, left, right);
      }
    }
  }
}

} // namespace lotus::cd::detail
