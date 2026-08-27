//===- CompactClosure.cpp - Incidence-based NTSCD-DOD closure ------------===//

#include "Analysis/ControlDependence/CompactControlDependence.h"

#include <cassert>
#include <deque>

namespace lotus::cd::detail {

NodeSet computeCompactDependencyClosure(Graph &graph, const NodeSet &seed,
                                        const DependenceResult &ntscd,
                                        const DODBicliqueMap &bicliques) {
  const size_t nodeCount = graph.size();
  std::vector<std::vector<GraphNode *>> ntscdByTarget(nodeCount + 1);
  std::vector<std::vector<GraphNode *>> leftOf(nodeCount + 1);
  std::vector<std::vector<GraphNode *>> rightOf(nodeCount + 1);

  for (const auto &entry : ntscd.first)
    for (GraphNode *decision : entry.second)
      ntscdByTarget[entry.first->getID()].push_back(decision);
  for (const auto &entry : bicliques) {
    GraphNode *decision = entry.first;
    for (unsigned id : entry.second.left)
      leftOf[id].push_back(decision);
    for (unsigned id : entry.second.right)
      rightOf[id].push_back(decision);
  }

  std::vector<bool> inClosure(nodeCount + 1, false);
  std::vector<bool> leftHit(nodeCount + 1, false);
  std::vector<bool> rightHit(nodeCount + 1, false);
  std::deque<GraphNode *> worklist;
  for (GraphNode *node : seed) {
    assert(node && node->getID() > 0 && node->getID() <= nodeCount);
    if (!inClosure[node->getID()]) {
      inClosure[node->getID()] = true;
      worklist.push_back(node);
    }
  }

  auto add = [&](GraphNode *node) {
    if (!inClosure[node->getID()]) {
      inClosure[node->getID()] = true;
      worklist.push_back(node);
    }
  };

  while (!worklist.empty()) {
    GraphNode *node = worklist.front();
    worklist.pop_front();
    const unsigned id = node->getID();
    for (GraphNode *decision : ntscdByTarget[id])
      add(decision);
    for (GraphNode *decision : leftOf[id]) {
      const unsigned decisionID = decision->getID();
      if (!leftHit[decisionID]) {
        leftHit[decisionID] = true;
        if (rightHit[decisionID])
          add(decision);
      }
    }
    for (GraphNode *decision : rightOf[id]) {
      const unsigned decisionID = decision->getID();
      if (!rightHit[decisionID]) {
        rightHit[decisionID] = true;
        if (leftHit[decisionID])
          add(decision);
      }
    }
  }

  NodeSet result;
  for (GraphNode *node : graph.nodes())
    if (inClosure[node->getID()])
      result.insert(node);
  return result;
}

NodeSet computeEagerPairDependencyClosure(Graph &graph, const NodeSet &seed,
                                          const DependenceResult &ntscd,
                                          const DODBicliqueMap &bicliques) {
  struct PairState {
    GraphNode *decision;
    GraphNode *first;
    GraphNode *second;
    bool firstHit{false};
    bool secondHit{false};
  };

  const size_t nodeCount = graph.size();
  std::vector<std::vector<GraphNode *>> ntscdByTarget(nodeCount + 1);
  for (const auto &entry : ntscd.first)
    for (GraphNode *decision : entry.second)
      ntscdByTarget[entry.first->getID()].push_back(decision);

  std::vector<PairState> pairs;
  size_t pairCount = 0;
  for (const auto &entry : bicliques)
    pairCount += entry.second.pairCount();
  pairs.reserve(pairCount);
  forEachDODPair(graph, bicliques,
                 [&](GraphNode *decision, GraphNode *first, GraphNode *second) {
                   pairs.push_back({decision, first, second});
                 });

  std::vector<std::vector<size_t>> pairsByEndpoint(nodeCount + 1);
  for (size_t index = 0; index < pairs.size(); ++index) {
    pairsByEndpoint[pairs[index].first->getID()].push_back(index);
    pairsByEndpoint[pairs[index].second->getID()].push_back(index);
  }

  std::vector<bool> inClosure(nodeCount + 1, false);
  std::deque<GraphNode *> worklist;
  for (GraphNode *node : seed) {
    assert(node && node->getID() > 0 && node->getID() <= nodeCount);
    if (!inClosure[node->getID()]) {
      inClosure[node->getID()] = true;
      worklist.push_back(node);
    }
  }

  auto add = [&](GraphNode *node) {
    if (!inClosure[node->getID()]) {
      inClosure[node->getID()] = true;
      worklist.push_back(node);
    }
  };

  while (!worklist.empty()) {
    GraphNode *node = worklist.front();
    worklist.pop_front();
    const unsigned id = node->getID();
    for (GraphNode *decision : ntscdByTarget[id])
      add(decision);
    for (size_t pairIndex : pairsByEndpoint[id]) {
      PairState &pair = pairs[pairIndex];
      if (pair.first == node)
        pair.firstHit = true;
      if (pair.second == node)
        pair.secondHit = true;
      if (pair.firstHit && pair.secondHit)
        add(pair.decision);
    }
  }

  NodeSet result;
  for (GraphNode *node : graph.nodes())
    if (inClosure[node->getID()])
      result.insert(node);
  return result;
}

} // namespace lotus::cd::detail
