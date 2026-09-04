#ifndef _DWGRAPH_UTIL_H_
#define _DWGRAPH_UTIL_H_

#include "CFL/CSIndex/FLARE/PathTree/WeightedGraph.h"

namespace lotus::cfl::cs_index::flare::path_tree {

// #define DEBUG

using EdgePtr = pair<DWEdgeList::iterator, DWEdgeList::iterator>;
using EdgePtrMap = vector<EdgePtr>;

class WeightedGraphAlgorithms {
public:
  //	static void dfs(WeightedGraph& g, int vid, vector<int>& preorder, vector<int>&
  //postorder); 	static void topological_sort(WeightedGraph g, vector<int>& ts); 	static
  //void traverse(WeightedGraph& tree, int vid, int& pre_post); 	static void
  //pre_post_labeling(WeightedGraph& tree);
  static void tarjan(WeightedGraph &g, int vid, int &index,
                     map<int, pair<int, int>> &order, vector<int> &sn,
                     map<int, vector<int>> &sccmap, int &scc);
  // Edmonds' Branching Algorithm
  static void findMaxBranching(WeightedGraph &g, WeightedGraph &branching);
  static void findMaxBranching1(WeightedGraph &g, WeightedGraph &branching);
  static bool checkBranch(WeightedGraph branch);
  static bool checkBranching(WeightedGraph &graph, WeightedGraph &branch);
  static bool checkBranching1(WeightedGraph &graph, WeightedGraph &branch);

  // for test
  static void genRandomGraph(int n, double c, const char *filename);
};


} // namespace lotus::cfl::cs_index::flare::path_tree

#endif
