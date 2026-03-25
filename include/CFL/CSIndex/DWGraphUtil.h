#ifndef _DWGRAPH_UTIL_H_
#define _DWGRAPH_UTIL_H_

#include "CFL/CSIndex/DWGraph.h"

// #define DEBUG

typedef pair<DWEdgeList::iterator, DWEdgeList::iterator> EdgePtr;
typedef vector<EdgePtr> EdgePtrMap;

class DWGraphUtil {
public:
  //	static void dfs(DWGraph& g, int vid, vector<int>& preorder, vector<int>&
  //postorder); 	static void topological_sort(DWGraph g, vector<int>& ts); 	static
  //void traverse(DWGraph& tree, int vid, int& pre_post); 	static void
  //pre_post_labeling(DWGraph& tree);
  static void tarjan(DWGraph &g, int vid, int &index,
                     map<int, pair<int, int>> &order, vector<int> &sn,
                     map<int, vector<int>> &sccmap, int &scc);
  // Edmonds' Branching Algorithm
  static void findMaxBranching(DWGraph &g, DWGraph &branching);
  static void findMaxBranching1(DWGraph &g, DWGraph &branching);
  static bool checkBranch(DWGraph branch);
  static bool checkBranching(DWGraph &graph, DWGraph &branch);
  static bool checkBranching1(DWGraph &graph, DWGraph &branch);

  // for test
  static void genRandomGraph(int n, double c, const char *filename);
};

#endif
