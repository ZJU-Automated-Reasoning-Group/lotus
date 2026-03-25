#include "IR/SVFG/SVFGStats.h"

#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <iostream>

using namespace lotus::analysis;

void SVFGStats::performStat() {
  clear();
  processGraph();
}

void SVFGStats::printStat(const std::string &title) {
  if (!title.empty()) {
    std::cout << "=== " << title << " ===\n";
  }

  std::cout << "=== SVFG Statistics ===\n";
  std::cout << "Total Nodes: " << numNodes << "\n";
  std::cout << "  FormalIn: " << numFormalIn << "\n";
  std::cout << "  FormalOut: " << numFormalOut << "\n";
  std::cout << "  FormalParam: " << numFormalParam << "\n";
  std::cout << "  FormalRet: " << numFormalRet << "\n";
  std::cout << "  ActualIn: " << numActualIn << "\n";
  std::cout << "  ActualOut: " << numActualOut << "\n";
  std::cout << "  ActualParam: " << numActualParam << "\n";
  std::cout << "  ActualRet: " << numActualRet << "\n";
  std::cout << "  Load: " << numLoad << "\n";
  std::cout << "  Store: " << numStore << "\n";
  std::cout << "  Copy: " << numCopy << "\n";
  std::cout << "  Gep: " << numGep << "\n";
  std::cout << "  Addr: " << numAddr << "\n";
  std::cout << "  MSSAPhi: " << numMSSAPhi << "\n";
  std::cout << "  Phi: " << numPhi << "\n";

  std::cout << "Total Edges: " << totalInEdge << "\n";
  std::cout << "  IndCall: " << totalIndCallEdge << "\n";
  std::cout << "  IndRet: " << totalIndRetEdge << "\n";
  std::cout << "  DirCall: " << totalDirCallEdge << "\n";
  std::cout << "  DirRet: " << totalDirRetEdge << "\n";

  std::cout << "Node Degrees:\n";
  std::cout << "  Avg In: " << avgInDegree << ", Avg Out: " << avgOutDegree
            << "\n";
  std::cout << "  Max In: " << maxInDegree << ", Max Out: " << maxOutDegree
            << "\n";
  std::cout << "  Avg Ind In: " << avgIndInDegree
            << ", Avg Ind Out: " << avgIndOutDegree << "\n";
  std::cout << "  Max Ind In: " << maxIndInDegree
            << ", Max Ind Out: " << maxIndOutDegree << "\n";

  if (!sccRep.empty()) {
    std::cout << "Strongly Connected Components:\n";
    std::cout << "  Total SCCs: " << getNumSCCs() << "\n";
    std::cout << "  Nodes in cycles: " << cycleNodes.size() << "\n";
  }
}

void SVFGStats::startTopLevelNodeTimer() {
  addTopLevelNodeTimeStart = std::chrono::high_resolution_clock::now();
}

void SVFGStats::stopTopLevelNodeTimer() {
  addTopLevelNodeTimeEnd = std::chrono::high_resolution_clock::now();
}

void SVFGStats::startAddrTakenNodeTimer() {
  addAddrTakenNodeTimeStart = std::chrono::high_resolution_clock::now();
}

void SVFGStats::stopAddrTakenNodeTimer() {
  addAddrTakenNodeTimeEnd = std::chrono::high_resolution_clock::now();
}

void SVFGStats::startDirVFEdgeTimer() {
  connectDirVFGEdgeTimeStart = std::chrono::high_resolution_clock::now();
}

void SVFGStats::stopDirVFEdgeTimer() {
  connectDirVFGEdgeTimeEnd = std::chrono::high_resolution_clock::now();
}

void SVFGStats::startIndVFEdgeTimer() {
  connectIndVFGEdgeTimeStart = std::chrono::high_resolution_clock::now();
}

void SVFGStats::stopIndVFEdgeTimer() {
  connectIndVFGEdgeTimeEnd = std::chrono::high_resolution_clock::now();
}

void SVFGStats::startOptTimer() {
  svfgOptTimeStart = std::chrono::high_resolution_clock::now();
}

void SVFGStats::stopOptTimer() {
  svfgOptTimeEnd = std::chrono::high_resolution_clock::now();
}

void SVFGStats::addToForwardSlice(const SVFGNode *node) {
  forwardSlice.insert(node);
}

void SVFGStats::addToBackwardSlice(const SVFGNode *node) {
  backwardSlice.insert(node);
}

bool SVFGStats::inForwardSlice(const SVFGNode *node) const {
  return forwardSlice.find(node) != forwardSlice.end();
}

bool SVFGStats::inBackwardSlice(const SVFGNode *node) const {
  return backwardSlice.find(node) != backwardSlice.end();
}

void SVFGStats::addSource(const SVFGNode *node) { sources.insert(node); }

void SVFGStats::addSink(const SVFGNode *node) { sinks.insert(node); }

bool SVFGStats::isSource(const SVFGNode *node) const {
  return sources.find(node) != sources.end();
}

bool SVFGStats::isSink(const SVFGNode *node) const {
  return sinks.find(node) != sinks.end();
}

void SVFGStats::performSCCAnalysis(const SVFGEdgeSet &insensitiveCalRetEdges) {
  sccRep.clear();
  cycleNodes.clear();

  // Tarjan SCC over node IDs using all edges.
  //
  // Use an explicit DFS stack rather than recursive calls: large SVFGs can
  // contain long acyclic stretches between cycle entries, and the recursive
  // version overflows the host stack when ContextDDA enables cycle-insensitive
  // edge handling on those graphs.
  std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
  adj.reserve(graph->getNumNodes());
  for (const auto &pair : *graph) {
    const SVFGNode *node = pair.second;
    if (!node)
      continue;
    auto &out = adj[node->getId()];
    out.reserve(node->getOutEdges().size());
    for (const SVFGEdge *edge : node->getOutEdges()) {
      if (insensitiveCalRetEdges.count(edge))
        continue;
      if (edge && edge->getDstNode()) {
        out.push_back(edge->getDstNode()->getId());
      }
    }
  }

  std::unordered_map<uint32_t, int> index;
  std::unordered_map<uint32_t, int> lowlink;
  std::unordered_set<uint32_t> onStack;
  std::vector<uint32_t> stack;
  stack.reserve(adj.size());
  int nextIndex = 0;

  struct Frame {
    uint32_t nodeId;
    size_t nextSuccIdx = 0;
    bool initialized = false;
  };
  std::vector<Frame> dfsStack;
  dfsStack.reserve(adj.size());
  static const std::vector<uint32_t> emptySuccs;

  for (const auto &pair : *graph) {
    const SVFGNode *node = pair.second;
    if (!node)
      continue;
    const uint32_t id = node->getId();
    if (index.find(id) != index.end())
      continue;

    dfsStack.push_back(Frame{id});
    while (!dfsStack.empty()) {
      Frame &frame = dfsStack.back();
      const uint32_t v = frame.nodeId;

      if (!frame.initialized) {
        index[v] = nextIndex;
        lowlink[v] = nextIndex;
        nextIndex++;
        stack.push_back(v);
        onStack.insert(v);
        frame.initialized = true;
      }

      const auto adjIt = adj.find(v);
      const std::vector<uint32_t> &succs =
          (adjIt != adj.end()) ? adjIt->second : emptySuccs;

      if (frame.nextSuccIdx < succs.size()) {
        const uint32_t w = succs[frame.nextSuccIdx++];
        if (index.find(w) == index.end()) {
          dfsStack.push_back(Frame{w});
          continue;
        }
        if (onStack.count(w))
          lowlink[v] = std::min(lowlink[v], index[w]);
        continue;
      }

      dfsStack.pop_back();
      if (!dfsStack.empty()) {
        const uint32_t parent = dfsStack.back().nodeId;
        lowlink[parent] = std::min(lowlink[parent], lowlink[v]);
      }

      if (lowlink[v] != index[v])
        continue;

      // Pop SCC rooted at v.
      std::vector<uint32_t> scc;
      while (!stack.empty()) {
        const uint32_t w = stack.back();
        stack.pop_back();
        onStack.erase(w);
        scc.push_back(w);
        if (w == v)
          break;
      }

      uint32_t rep = scc.front();
      for (uint32_t member : scc)
        rep = std::min(rep, member);
      for (uint32_t member : scc)
        sccRep[member] = rep;

      if (scc.size() > 1) {
        cycleNodes.insert(scc.begin(), scc.end());
        continue;
      }

      // Single-node SCC: check self-loop.
      const auto repIt = adj.find(rep);
      if (repIt == adj.end())
        continue;
      for (uint32_t succ : repIt->second) {
        if (succ == rep) {
          cycleNodes.insert(rep);
          break;
        }
      }
    }
  }
}

uint32_t SVFGStats::getSCCRepNode(uint32_t nodeId) const {
  auto it = sccRep.find(nodeId);
  return (it != sccRep.end()) ? it->second : nodeId;
}

bool SVFGStats::isEdgeInSVFGSCC(const SVFGEdge *edge) const {
  if (!edge || !graph)
    return false;
  const SVFGNode *src = edge->getSrcNode();
  const SVFGNode *dst = edge->getDstNode();
  if (!src || !dst)
    return false;
  return getSCCRepNode(src->getId()) == getSCCRepNode(dst->getId());
}

void SVFGStats::clear() {
  numNodes = 0;
  numFormalIn = numFormalOut = numFormalParam = numFormalRet = 0;
  numActualIn = numActualOut = numActualParam = numActualRet = 0;
  numLoad = numStore = numCopy = numGep = numAddr = 0;
  numMSSAPhi = numPhi = 0;

  totalInEdge = totalOutEdge = totalIndInEdge = totalIndOutEdge = 0;
  totalIndEdgeLabels = 0;
  totalIndCallEdge = totalIndRetEdge = totalDirCallEdge = totalDirRetEdge = 0;

  avgInDegree = avgOutDegree = 0;
  maxInDegree = maxOutDegree = 0;
  avgIndInDegree = avgIndOutDegree = 0;
  maxIndInDegree = maxIndOutDegree = 0;
  avgWeight = 0;

  forwardSlice.clear();
  backwardSlice.clear();
  sources.clear();
  sinks.clear();

  sccRep.clear();
  cycleNodes.clear();
}

void SVFGStats::processGraph() {
  SVFGNodeSet nodesWithIndInEdge;
  SVFGNodeSet nodesWithIndOutEdge;

  for (const auto &pair : *graph) {
    const SVFGNode *node = pair.second;
    numNodes++;

    switch (node->getNodeKind()) {
    case SVFGK::FormalIn:
      numFormalIn++;
      break;
    case SVFGK::FormalOut:
      numFormalOut++;
      break;
    case SVFGK::FormalParm:
      numFormalParam++;
      break;
    case SVFGK::FormalRet:
      numFormalRet++;
      break;
    case SVFGK::ActualIn:
      numActualIn++;
      break;
    case SVFGK::ActualOut:
      numActualOut++;
      break;
    case SVFGK::ActualParm:
      numActualParam++;
      break;
    case SVFGK::ActualRet:
      numActualRet++;
      break;
    case SVFGK::Load:
      numLoad++;
      break;
    case SVFGK::Store:
      numStore++;
      break;
    case SVFGK::Copy:
      numCopy++;
      break;
    case SVFGK::Gep:
      numGep++;
      break;
    case SVFGK::Addr:
      numAddr++;
      break;
    case SVFGK::MPhi:
    case SVFGK::MIntraPhi:
    case SVFGK::MInterPhi:
      numMSSAPhi++;
      break;
    case SVFGK::Phi:
    case SVFGK::IntraPhi:
    case SVFGK::InterPhi:
      numPhi++;
      break;
    default:
      break;
    }

    for (const SVFGEdge *edge : node->getOutEdges()) {
      totalOutEdge++;

      switch (edge->getEdgeKind()) {
      case SVFGEdgeK::CallDir:
      case SVFGEdgeK::ParamCall:
      case SVFGEdgeK::CallAIn:
      case SVFGEdgeK::CallFIn:
        totalDirCallEdge++;
        break;
      case SVFGEdgeK::CallInd:
        totalIndCallEdge++;
        break;
      case SVFGEdgeK::RetDir:
      case SVFGEdgeK::ParamRet:
      case SVFGEdgeK::RetAOut:
      case SVFGEdgeK::RetFOut:
        totalDirRetEdge++;
        break;
      case SVFGEdgeK::RetInd:
        totalIndRetEdge++;
        break;
      default:
        break;
      }

      if (!edge->getPointsTo().empty()) {
        totalIndEdgeLabels++;
      }
    }

    for (auto it = node->getInEdges().begin(); it != node->getInEdges().end();
         ++it) {
      totalInEdge++;
    }

    calculateNodeDegrees(node, nodesWithIndInEdge, nodesWithIndOutEdge);
  }

  if (numNodes > 0) {
    avgInDegree = totalInEdge / numNodes;
    avgOutDegree = totalOutEdge / numNodes;
    avgIndInDegree = totalIndInEdge / numNodes;
    avgIndOutDegree = totalIndOutEdge / numNodes;
  }
}

void SVFGStats::calculateNodeDegrees(const SVFGNode *node,
                                     SVFGNodeSet &nodesWithIndInEdge,
                                     SVFGNodeSet &nodesWithIndOutEdge) {
  (void)nodesWithIndInEdge;
  (void)nodesWithIndOutEdge;

  uint32_t inDegree = node->getInEdges().size();
  uint32_t outDegree = node->getOutEdges().size();

  maxInDegree = std::max(maxInDegree, inDegree);
  maxOutDegree = std::max(maxOutDegree, outDegree);

  uint32_t indInDegree = 0;
  uint32_t indOutDegree = 0;

  auto isIndirectEdge = [](const SVFGEdge *edge) {
    if (!edge)
      return false;
    if (edge->getEdgeKind() == SVFGEdgeK::CallInd ||
        edge->getEdgeKind() == SVFGEdgeK::RetInd) {
      return true;
    }
    return !edge->getPointsTo().empty();
  };

  for (const SVFGEdge *edge : node->getInEdges()) {
    if (isIndirectEdge(edge)) {
      indInDegree++;
      nodesWithIndInEdge.insert(node);
    }
  }

  for (const SVFGEdge *edge : node->getOutEdges()) {
    if (isIndirectEdge(edge)) {
      indOutDegree++;
      nodesWithIndOutEdge.insert(node);
    }
  }

  totalIndInEdge += indInDegree;
  totalIndOutEdge += indOutDegree;

  maxIndInDegree = std::max(maxIndInDegree, indInDegree);
  maxIndOutDegree = std::max(maxIndOutDegree, indOutDegree);
}

uint32_t SVFGStats::getSCCRep(uint32_t nodeId) {
  auto it = sccRep.find(nodeId);
  if (it != sccRep.end())
    return it->second;
  return nodeId;
}

bool SVFGStats::nodeInCycle(uint32_t nodeId) {
  return cycleNodes.count(nodeId) != 0;
}

uint32_t SVFGStats::getSCCSize(uint32_t nodeId) const {
  uint32_t rep = getSCCRepNode(nodeId);
  uint32_t count = 0;
  for (const auto &pair : sccRep) {
    if (pair.second == rep) {
      count++;
    }
  }
  return count;
}

std::vector<uint32_t> SVFGStats::getNodesInSCC(uint32_t nodeId) const {
  std::vector<uint32_t> result;
  uint32_t rep = getSCCRepNode(nodeId);
  for (const auto &pair : sccRep) {
    if (pair.second == rep) {
      result.push_back(pair.first);
    }
  }
  return result;
}

uint32_t SVFGStats::getNumSCCs() const {
  std::unordered_set<uint32_t> uniqueReps;
  for (const auto &pair : sccRep) {
    uniqueReps.insert(pair.second);
  }
  return uniqueReps.size();
}
