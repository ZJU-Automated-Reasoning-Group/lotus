#ifndef _PATH_TREE_H
#define _PATH_TREE_H

#include "CFL/CSIndex/FLARE/PathTree/WeightedGraphAlgorithms.h"
#include "CFL/CSIndex/FLARE/PathTree/DataCompression.h"
#include "CFL/CSIndex/FLARE/GraphAlgorithms.h"

namespace lotus::cfl::cs_index::flare::path_tree {

// test switch
#define _TEST_

/**
 * @brief Path tree implementation for reachability queries.
 *
 * This class implements path tree-based indexing for efficient reachability
 * queries using weighted path graphs and equivalent graphs.
 */
class Index {
public:
  Graph &g;        ///< Reference to the main graph
  WeightedGraph pg;      ///< Path graph
  Graph ng;        ///< Equivalent weight graph
  WeightedGraph branch;  ///< Branch graph
  Graph newbranch; ///< New branch graph
  int maxeid;      ///< Maximum edge ID

  vector<int> nextVertex;           ///< Next vertex mapping
  vector<vector<int>> out_uncover;  ///< Uncovered outgoing edges
  map<int, vector<int>> comp_table; ///< Component table
  vector<vector<int>> pathMap;      ///< Path mapping
  vector<int> grts;                 ///< Graph reverse topological sort
  int **labels;                     ///< Labeling structure
  bool effective;                   ///< Effectiveness flag

  map<pair<int, int>, bool> tcm;          ///< Test coverage map
  struct timeval after_time, before_time; ///< Timing structures
  float run_time;                         ///< Runtime measurement

public:
  /**
   * @brief Constructor with graph reference.
   * @param graph Reference to the main graph
   */
  Index(Graph &graph);

  /**
   * @brief Constructor with graph reference and topological sort.
   * @param graph Reference to the main graph
   * @param ts Topological sort vector
   */
  Index(Graph &graph, vector<int> ts);

  /**
   * @brief Destructor.
   */
  ~Index();

  /**
   * @brief Build weighted path graph.
   * @param type Type of path graph construction
   */
  void buildWeightPathGraph(int type);

  /**
   * @brief Build weighted path graph with predecessor-based weights.
   */
  void buildWeightPathGraph_Pred();

  /**
   * @brief Build equivalent graph.
   */
  void buildEquGraph();

  /**
   * @brief Create labels for indexing.
   * @param type Label type
   * @param cfile Input file for compression data
   * @param compress Compression flag
   */
  void createLabels(int type, ifstream &cfile, bool compress);

  /**
   * @brief Display labels.
   */
  void displayLabels();

  /**
   * @brief Perform path DFS traversal.
   * @param vid Starting vertex ID
   * @param order Order counter
   * @param first_order First order counter
   * @param visited Visited flags vector
   */
  void pathDFS(int vid, int &order, int &first_order, vector<bool> &visited);

  /**
   * @brief Transform directed weighted graph to regular graph.
   * @param dg Directed weighted graph
   * @param graph Output graph
   */
  void transform(WeightedGraph dg, Graph &graph);

  /**
   * @brief Read path map from file.
   * @param cfile Input file stream
   */
  void readPathMap(ifstream &cfile);

  /**
   * @brief Compute test coverage map.
   */
  void compute_tcm();

  /**
   * @brief Check reachability between source and target.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool reach(int src, int trg);

  /**
   * @brief Check reachability with data compression.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool reach_dc(int src, int trg);

  /**
   * @brief Test reachability with validation.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool test_reach(int src, int trg);

  /**
   * @brief Test reachability with data compression and validation.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool test_reach_dc(int src, int trg);

  /**
   * @brief Get index size information.
   * @param ind_size Array to store index sizes
   */
  void index_size(int *ind_size);

  /**
   * @brief Insert elements from one set into another.
   * @param s1 Source set
   * @param s2 Destination set
   */
  void insertSet(set<int> &s1, set<int> &s2);

  /**
   * @brief Merge two vectors.
   * @param v1 First vector
   * @param v2 Second vector
   */
  void mergeVector(vector<int> &v1, vector<int> &v2);

  /**
   * @brief Build equivalent edge set.
   * @param pathtopo Path topology map
   * @param equgraph Equivalent graph
   */
  void buildEquEdgeset(map<int, set<int>> &pathtopo, Graph &equgraph);

  /**
   * @brief Get coverage ratio.
   * @return Coverage ratio value
   */
  double cover_ratio();

  /**
   * @brief Get compression ratio.
   * @return Compression ratio value
   */
  double compress_ratio();

  /**
   * @brief Save labels to file.
   * @param labels_file Output file stream
   */
  void save_labels(ofstream &labels_file);
};


} // namespace lotus::cfl::cs_index::flare::path_tree

#endif
