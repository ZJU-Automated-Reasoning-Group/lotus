#pragma once

#include "IR/SVFG/SVFG.h"

#include <chrono>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus {
namespace analysis {

class SVFGStats {
public:
  using SVFGNodeSet = std::set<const SVFGNode *>;
  using SVFGEdgeSet = std::set<const SVFGEdge *>;

  SVFGStats(SVFG *graph) : graph(graph) {}

  ~SVFGStats() = default;

  void performStat();
  void printStat(const std::string &title = "");

  void startTopLevelNodeTimer();
  void stopTopLevelNodeTimer();
  void startAddrTakenNodeTimer();
  void stopAddrTakenNodeTimer();
  void startDirVFEdgeTimer();
  void stopDirVFEdgeTimer();
  void startIndVFEdgeTimer();
  void stopIndVFEdgeTimer();
  void startOptTimer();
  void stopOptTimer();

  void addToForwardSlice(const SVFGNode *node);
  void addToBackwardSlice(const SVFGNode *node);
  bool inForwardSlice(const SVFGNode *node) const;
  bool inBackwardSlice(const SVFGNode *node) const;

  void addSource(const SVFGNode *node);
  void addSink(const SVFGNode *node);
  bool isSource(const SVFGNode *node) const;
  bool isSink(const SVFGNode *node) const;

  void performSCCAnalysis(const SVFGEdgeSet &insensitiveCalRetEdges);

  /// @brief SCC representative node ID (for DDA context-insensitive edges).
  /// Only valid after performSCCAnalysis() has been called.
  uint32_t getSCCRepNode(uint32_t nodeId) const;
  /// @brief True if \p edge's src and dst are in the same SVFG SCC.
  bool isEdgeInSVFGSCC(const SVFGEdge *edge) const;

  /// @brief Get number of nodes in a strongly connected component
  uint32_t getSCCSize(uint32_t nodeId) const;

  /// @brief Get all nodes in the same SCC as the given node
  std::vector<uint32_t> getNodesInSCC(uint32_t nodeId) const;

  /// @brief Get number of SCCs in the graph
  uint32_t getNumSCCs() const;

 private:
  void clear();
  void processGraph();
  void calculateNodeDegrees(const SVFGNode *node,
                            SVFGNodeSet &nodesWithIndInEdge,
                            SVFGNodeSet &nodesWithIndOutEdge);
  uint32_t getSCCRep(uint32_t nodeId);
  bool nodeInCycle(uint32_t nodeId);

  SVFG *graph;

  std::chrono::high_resolution_clock::time_point addTopLevelNodeTimeStart;
  std::chrono::high_resolution_clock::time_point addTopLevelNodeTimeEnd;
  std::chrono::high_resolution_clock::time_point addAddrTakenNodeTimeStart;
  std::chrono::high_resolution_clock::time_point addAddrTakenNodeTimeEnd;
  std::chrono::high_resolution_clock::time_point connectDirVFGEdgeTimeStart;
  std::chrono::high_resolution_clock::time_point connectDirVFGEdgeTimeEnd;
  std::chrono::high_resolution_clock::time_point connectIndVFGEdgeTimeStart;
  std::chrono::high_resolution_clock::time_point connectIndVFGEdgeTimeEnd;
  std::chrono::high_resolution_clock::time_point svfgOptTimeStart;
  std::chrono::high_resolution_clock::time_point svfgOptTimeEnd;

  int numNodes;
  int numFormalIn;
  int numFormalOut;
  int numFormalParam;
  int numFormalRet;
  int numActualIn;
  int numActualOut;
  int numActualParam;
  int numActualRet;
  int numLoad;
  int numStore;
  int numCopy;
  int numGep;
  int numAddr;
  int numMSSAPhi;
  int numPhi;

  int totalInEdge;
  int totalOutEdge;
  int totalIndInEdge;
  int totalIndOutEdge;
  int totalIndEdgeLabels;

  int totalIndCallEdge;
  int totalIndRetEdge;
  int totalDirCallEdge;
  int totalDirRetEdge;

  int avgInDegree;
  int avgOutDegree;
  uint32_t maxInDegree;
  uint32_t maxOutDegree;

  int avgIndInDegree;
  int avgIndOutDegree;
  uint32_t maxIndInDegree;
  uint32_t maxIndOutDegree;

  int avgWeight;

  SVFGNodeSet forwardSlice;
  SVFGNodeSet backwardSlice;
  SVFGNodeSet sources;
  SVFGNodeSet sinks;

  // SCC/cycle results
  std::unordered_map<uint32_t, uint32_t> sccRep;
  std::unordered_set<uint32_t> cycleNodes;
};

} // namespace analysis
} // namespace lotus
