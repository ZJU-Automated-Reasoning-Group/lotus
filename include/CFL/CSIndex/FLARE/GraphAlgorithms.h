#ifndef _GRAPH_UTIL_H_
#define _GRAPH_UTIL_H_

#include "CFL/CSIndex/FLARE/Graph.h"

#include <sys/time.h>

namespace lotus::cfl::cs_index::flare {

class GraphAlgorithms {
public:
  static void dfs(Graph &g, int vid, vector<int> &preorder,
                  vector<int> &postorder, vector<bool> &visited);
  static void topological_sort(Graph &g, vector<int> &ts);
  static void topo_leveler(Graph &g);
  static int topo_level(Graph &g, int vid);
  static void transitive_closure(Graph g, Graph &tc);
  static void tarjan(Graph &g, int vid, int &index,
                     unordered_map<int, pair<int, int>> &order, vector<int> &sn,
                     multimap<int, int> &sccmap, int &scc);
  static void mergeSCC(Graph &g, int *on, vector<int> &ts);
  static void findTreeCover(Graph g, Graph &tree);
  static void findTreeCover(Graph g, Graph &tree, vector<set<int>> &pred);
  static void findTreeCover(Graph &g, Graph &tree, vector<set<int>> &pred,
                            vector<int> &ts);
  static void compute_pred(Graph g, vector<set<int>> &predMap);
  static void findTreeCoverL(Graph g, Graph &tree);
  static void traverse(Graph &tree, int vid, int &pre_post,
                       vector<bool> &visited);
  static void pre_post_labeling(Graph &tree);
  static void pathDecomposition(Graph &g, vector<vector<int>> &pathMap);
  static void pathDecomposition(Graph &g, vector<vector<int>> &pathMap,
                                vector<int> ts);
  static void treePathDecomposition(Graph tree, Graph &g,
                                    vector<vector<int>> &pathMap);

  static void genRandomGraph(int n, double c, char *filename);

  static bool DFSCheck(Graph &graph, int vid, BitVector *visited, int trg);
  static bool DFSReach(Graph &graph, int src, int trg);

  static void buildGateGraphByNodeFast(Graph &g, BitVector *isgates, int vid,
                                       int radius, map<int, int> &gateindex,
                                       Graph &gategraph, vector<int> &que,
                                       vector<int> &dist, int &ref);
  static int buildGateGraphByNodeFastWrite(Graph &g, BitVector *isgates,
                                           int vid, int radius,
                                           map<int, int> &gateindex,
                                           ostream &out, vector<int> &que,
                                           vector<int> &dist, int &ref);
  static int buildGateGraphWrite(Graph &graph, BitVector *isgate, int radius,
                                 ostream &out);

  static bool DFSCheckCnt(Graph &graph, int vid, vector<int> &visited, int trg,
                          int qcnt);
  static bool DFSReachCnt(Graph &graph, int src, int trg, vector<int> &visited,
                          int &qcnt);

  static void collectInLocalGates(Graph &graph, const BitVector *isgates,
                                  int vid, int radius, vector<int> &ingates);
  static bool BFSOutLocalReach(Graph &graph, const BitVector *isgates,
                               int radius, int src, int trg,
                               vector<int> &outgates);
  static void collectOutLocalGates(Graph &graph, const BitVector *isgates,
                                   int vid, int radius, vector<int> &outgates);
  static int visit(Graph &tree, int vid, int &pre_post, vector<bool> &visited,
                   vector<pair<int, int>> &dfslabels);
  static void grail_labeling(Graph &tree, vector<pair<int, int>> &dfslabels);
};


} // namespace lotus::cfl::cs_index::flare

#endif
