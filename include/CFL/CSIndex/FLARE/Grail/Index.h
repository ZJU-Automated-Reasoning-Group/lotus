/* Copyright (c) Hilmi Yildirim 2010,2011.

The software is provided on an as is basis for research purposes.
There is no additional support offered, nor are the author(s)
or their institutions liable under any circumstances.
*/
#ifndef _BOX_H
#define _BOX_H

#include "CFL/CSIndex/FLARE/ReachabilityQuery.h"
#include "CFL/CSIndex/FLARE/Grail/ExceptionList.h"
#include "CFL/CSIndex/FLARE/GraphAlgorithms.h"

namespace lotus::cfl::cs_index::flare::grail {

// test switch
#define _TEST_

/**
 * @brief GRAIL (GRAph Indexing and Labeling) implementation for reachability
 * queries.
 *
 * This class implements the GRAIL algorithm which uses multiple random labeling
 * to answer reachability queries efficiently with high accuracy.
 */
class Index : public ReachabilityQuery {
public:
  Graph &g;                               ///< Reference to the main graph
  struct timeval after_time, before_time; ///< Timing structures
  float run_time;                         ///< Runtime measurement
  int dim;                                ///< Dimension for labeling
  int *visited;                           ///< Visited array for traversal
  int QueryCnt;                           ///< Query counter
  bool LEVEL_FILTER;                      ///< Level filtering flag
  bool POOL;                              ///< Pool usage flag
  int POOLSIZE;                           ///< Pool size
  unsigned int PositiveCut, NegativeCut, TotalCall, TotalDepth,
      CurrentDepth; ///< Statistics counters
public:
  /**
   * @brief Constructor with graph and parameters.
   * @param graph Reference to the main graph
   * @param dim Dimension for labeling
   * @param labelingType Type of labeling to use
   * @param POOL Pool usage flag
   * @param POOLSIZE Pool size
   */
  Index(Graph &graph, int dim, int labelingType, bool POOL, int POOLSIZE);

  /**
   * @brief Destructor.
   */
  ~Index();

  /**
   * @brief Visit function for DFS traversal.
   * @param tree Graph to traverse
   * @param vid Starting vertex ID
   * @param pre_post Pre/post order counter
   * @param visited Visited flags vector
   * @return Order number
   */
  static int visit(Graph &tree, int vid, int &pre_post, vector<bool> &visited);

  /**
   * @brief Fixed reverse visit function for DFS traversal.
   * @param tree Graph to traverse
   * @param vid Starting vertex ID
   * @param pre_post Pre/post order counter
   * @param visited Visited flags vector
   * @param traversal Traversal type
   * @return Order number
   */
  static int fixedreversevisit(Graph &tree, int vid, int &pre_post,
                               vector<bool> &visited, int traversal);

  /**
   * @brief Custom visit function for DFS traversal.
   * @param tree Graph to traverse
   * @param vid Starting vertex ID
   * @param pre_post Pre/post order counter
   * @param visited Visited flags vector
   * @param traversal Traversal type
   * @return Order number
   */
  static int customvisit(Graph &tree, int vid, int &pre_post,
                         vector<bool> &visited, int traversal);

  /**
   * @brief Perform random labeling on the graph.
   * @param tree Graph to label
   */
  static void randomlabeling(Graph &tree);

  /**
   * @brief Perform custom labeling on the graph.
   * @param tree Graph to label
   * @param traversal Traversal type
   */
  static void customlabeling(Graph &tree, int traversal);

  /**
   * @brief Perform fixed reverse labeling on the graph.
   * @param tree Graph to label
   * @param traversal Traversal type
   */
  static void fixedreverselabeling(Graph &tree, int traversal);

  /**
   * @brief Set index for the graph.
   * @param tree Graph to index
   * @param traversal Traversal type
   */
  static void setIndex(Graph &tree, int traversal);

  /**
   * @brief Set custom index for the graph.
   * @param tree Graph to index
   * @param traversal Traversal type
   * @param type Index type
   */
  static void setCustomIndex(Graph &tree, int traversal, int type);

  /**
   * @brief Set level filter flag.
   * @param lf Level filter flag value
   */
  void set_level_filter(bool lf);

  /**
   * @brief Check reachability with level filtering.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool reach_lf(int src, int trg, ExceptionList *el);

  /**
   * @brief Check bidirectional reachability.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool bidirectionalReach(int src, int trg, ExceptionList *el);

  /**
   * @brief Check bidirectional reachability with level filtering.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool bidirectionalReach_lf(int src, int trg, ExceptionList *el);

  /**
   * @brief Check reachability with post-processing.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool reachPP(int src, int trg, ExceptionList *el);

  /**
   * @brief Check reachability with post-processing and level filtering.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool reachPP_lf(int src, int trg, ExceptionList *el);

  /**
   * @brief Check bidirectional reachability with post-processing.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool bidirectionalReachPP(int src, int trg, ExceptionList *el);

  /**
   * @brief Check bidirectional reachability with post-processing and level
   * filtering.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @param el Exception list
   * @return true if reachable, false otherwise
   */
  bool bidirectionalReachPP_lf(int src, int trg, ExceptionList *el);

  /**
   * @brief Go for reachability check.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool go_for_reach(int src, int trg);

  /**
   * @brief Go for reachability check with level filtering.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool go_for_reach_lf(int src, int trg);

  /**
   * @brief Go for reachability check with post-processing.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool go_for_reachPP(int src, int trg);

  /**
   * @brief Go for reachability check with post-processing and level filtering.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool go_for_reachPP_lf(int src, int trg);

  /**
   * @brief Check if source contains target.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return true if contains, false otherwise
   */
  bool contains(int src, int trg);

  /**
   * @brief Check if source contains target with post-processing.
   * @param src Source vertex ID
   * @param trg Target vertex ID
   * @return 1 if contains, 0 otherwise
   */
  int containsPP(int src, int trg);

public:
  bool reach(int src, int dst) override {
    return reachPP_lf(src, dst, nullptr);
  }

  const char *method() const override { return "Index"; }

  void reset() override {}
};


} // namespace lotus::cfl::cs_index::flare::grail

#endif
