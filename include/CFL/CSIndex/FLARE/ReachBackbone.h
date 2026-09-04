#ifndef _REACH_BACKBONE_H_
#define _REACH_BACKBONE_H_

#include "CFL/CSIndex/FLARE/GraphAlgorithms.h"

#include <fstream>

namespace lotus::cfl::cs_index::flare {

// #define RBDEBUG
// #define RBDEBUG1

using namespace std;

class ReachBackbone {
private:
  Graph g;
  Graph gategraph;
  int gsize, epsilon, bbedgesize, level, blocknum;
  double preselectratio;
  set<int> bbvertices;

  // uility structures
  int ref, opCnt;
  BitVector *gates, *materialized;
  vector<vector<BitVector *>> localneighbors;
  vector<int> que, que2, visited, dist, life;

public:
  ReachBackbone(Graph &graph, int r, double ratio, int _level);
  ~ReachBackbone();
  int BFSNeighbors(int vid, int step, bool out);
  int MaterializeNeighbors(int vid, int step, bool out, BitVector *neighbors);
  void vertexRanking(vector<int> &ranks, int type);
  void backboneDiscovery(int type);
  bool backboneByNode(int vid);
  set<int> getBBvertices() const;
  void outputBackbone(const char *filestem);
  void printBBGraph(ostream &out);
  void printBBvertex(ostream &out);
  void setBlockNum(int _bn);
  void blockOrdering(vector<int> &ranks);
  int getBBsize() const;
  int getBBEdgesize() const;
  int genGateGraph(ostream &out, int radius, bool check);
  int genGateGraphByNode(int vid, int radius, ostream &out, bool check,
                         map<int, int> &gateindex);

  // for mock test
  void setbbvertex(set<int> bbv);
  void setMaterializedNodes(vector<int> hubnodes);
  bool reach(int src, int trg, map<int, int> gateindex);
  bool checkBackbone();
};


} // namespace lotus::cfl::cs_index::flare

#endif
