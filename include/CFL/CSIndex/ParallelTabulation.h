
#ifndef _PARALLEL_TABULATION_H
#define _PARALLEL_TABULATION_H

#include "CFL/CSIndex/AbstractQuery.h"
#include "CFL/CSIndex/Graph.h"

#include <set>
#include <vector>

class ParallelTabulation : public AbstractQuery {
private:
  Graph &vfg;
  size_t num_threads;

public:
  /**
   * @brief Constructor with graph reference and automatic thread detection
   * @param g Reference to the graph
   */
  explicit ParallelTabulation(Graph &g);

  /**
   * @brief Constructor with graph reference and specified number of threads
   * @param g Reference to the graph
   * @param threads Number of threads to use
   */
  ParallelTabulation(Graph &g, size_t threads);

  /**
   * @brief Check reachability between source and target vertices
   * @param s Source vertex ID
   * @param t Target vertex ID
   * @return true if reachable, false otherwise
   */
  bool reach(int s, int t) override;

  /**
   * @brief Check reachability within function body between source and target
   * vertices
   * @param s Source vertex ID
   * @param t Target vertex ID
   * @param visited Reachability-local visited set for function-body traversal
   * @return true if reachable, false otherwise
   */
  bool reach_func(int s, int t, std::set<int> &visited);

  /**
   * @brief Check if edge represents a function call
   * @param s Source vertex ID
   * @param t Target vertex ID
   * @return true if call edge, false otherwise
   */
  bool is_call(int s, int t);

  /**
   * @brief Check if edge represents a function return
   * @param s Source vertex ID
   * @param t Target vertex ID
   * @return true if return edge, false otherwise
   */
  bool is_return(int s, int t);

  /**
   * @brief Compute transitive closure for all vertices (parallel version)
   * @return Memory usage in MB
   */
  double tc();

  /**
   * @brief Parallel traversal from source vertex
   * @param s Source vertex ID
   * @param tc Set to store reachable vertices
   * @param visited Reachability-local visited set for interprocedural traversal
   * @param func_visited Reachability-local visited set for function-body traversal
   */
  void traverse_parallel(int s, std::set<int> &tc, std::set<int> &visited,
                         std::set<int> &func_visited);

  /**
   * @brief Parallel traversal within function body from source vertex
   * @param s Source vertex ID
   * @param tc Set to store reachable vertices
   * @param visited Reachability-local visited set for function-body traversal
   */
  void traverse_func_parallel(int s, std::set<int> &tc, std::set<int> &visited);

  /**
   * @brief Get the method name
   * @return Method name string
   */
  const char *method() const override;

  /**
   * @brief Reset the internal state
   */
  void reset() override;
};

#endif //_PARALLEL_TABULATION_H
