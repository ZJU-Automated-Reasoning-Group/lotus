/**
 * @file Sequential.cpp
 * @brief Implementation of tabulation-based CFL reachability algorithm.
 *
 * The Sequential class implements a context-sensitive reachability algorithm
 * that respects the Extended Dyck-CFL grammar:
 * - Positive labels represent call edges (entering functions)
 * - Negative labels represent return edges (exiting functions)
 * - Unlabeled edges represent intra-procedural flow
 *
 * Algorithm:
 * The tabulation algorithm performs a depth-first traversal that:
 * 1. Tracks visited vertices to avoid cycles
 * 2. Separately tracks function-visited vertices for inter-procedural paths
 * 3. Respects call-return matching: when entering a function via a call edge,
 *    it must exit via a matching return edge before continuing
 *
 * This is used for computing transitive closure (TC) estimates and for
 * validating the correctness of indexing structures.
 *
 * Time complexity: O(n * m) where n is vertices and m is edges in worst case.
 */

#include "CFL/CSIndex/FLARE/Tabulation/Sequential.h"

#include "Utils/Platform/ProgressBar.h"

#include <csignal>

#include <unistd.h>

namespace lotus::cfl::cs_index::flare::tabulation {

static bool timeout = false;

static void alarm_handler(int param) { timeout = true; }

/**
 * @brief Constructor for Sequential.
 * @param g Reference to the graph for reachability queries
 */
Sequential::Sequential(Graph &g) : vfg(g) {}

/**
 * @brief Check if vertex s can reach vertex t respecting CFL grammar.
 *
 * This is the main reachability query that respects context-sensitive flow:
 * - When encountering a call edge (positive label), enter the function body
 * - When encountering a return edge (negative label), exit the function
 * - Unlabeled edges represent intra-procedural flow
 *
 * @param s Source vertex
 * @param t Target vertex
 * @return true if s can reach t via a valid CFL path, false otherwise
 */
bool Sequential::reach(int s, int t) {
  if (visited.count(s))
    return false;

  if (s == t)
    return true;

  visited.insert(s);
  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_call(s, successor)) {
      // Enter function body: must exit via matching return before continuing
      if (reach_func(successor, t))
        return true;
    } else {
      // Intra-procedural edge or return edge (handled in reach_func)
      if (reach(successor, t))
        return true;
    }
  }

  return false;
}

/**
 * @brief Reachability within a function body (between call and return).
 *
 * This function is called when we've entered a function via a call edge.
 * It explores the function body but skips return edges (which would exit
 * the function prematurely). The function body traversal continues until
 * either the target is found or all paths are exhausted.
 *
 * @param s Current vertex within function body
 * @param t Target vertex
 * @return true if t is reachable from s within the function, false otherwise
 */
bool Sequential::reach_func(int s, int t) {
  if (func_visited.count(s))
    return false;
  if (s == t)
    return true;
  func_visited.insert(s);
  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_return(s, successor)) {
      // Skip return edges: we're still exploring the function body
      continue;
    } else {
      // Continue exploring function body
      if (reach_func(successor, t))
        return true;
    }
  }

  return false;
}

bool Sequential::is_call(int s, int t) { return vfg.label(s, t) > 0; }

bool Sequential::is_return(int s, int t) { return vfg.label(s, t) < 0; }

void Sequential::traverse(int s, std::set<int> &tc) {
  if (visited.count(s))
    return;
  if (timeout)
    return;

  visited.insert(s);
  tc.insert(s);

  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_call(s, successor)) {
      // visit the func body
      traverse_func(successor, tc);
    } else {
      traverse(successor, tc);
    }
  }
}

void Sequential::traverse_func(int s, std::set<int> &tc) {
  if (func_visited.count(s))
    return;
  if (timeout)
    return;

  func_visited.insert(s);
  tc.insert(s);

  auto &edges = vfg.out_edges(s);
  for (auto successor : edges) {
    if (is_return(s, successor)) {
      continue;
    } else {
      traverse_func(successor, tc);
    }
  }
}

/**
 * @brief Compute transitive closure (TC) size estimate.
 *
 * Computes the reachable set for each vertex and estimates the memory
 * required to store the full transitive closure. This is used for:
 * - Evaluating indexing effectiveness (compression ratio)
 * - Estimating memory requirements
 * - Performance benchmarking
 *
 * The algorithm performs a traversal from each vertex, collecting all
 * reachable vertices. The result is the total memory (in MB) needed to
 * store all reachability relationships.
 *
 * @return Estimated transitive closure size in megabytes
 */
double Sequential::tc() {
  signal(SIGALRM, alarm_handler);
  timeout = false;
  alarm(3600 * 6); // 6 hour timeout
  ProgressBar bar("Sequential tabulation", ProgressBar::PBS_CharacterStyle);

  double ret = 0;
  std::map<int, std::set<int>> tc;
  for (int i = 0; i < vfg.num_vertices(); ++i) {
    visited.clear();
    func_visited.clear();
    traverse(i, tc[i]);
    ret += (tc[i].size()) * sizeof(int);
    bar.showProgress(static_cast<float>(i + 1) / vfg.num_vertices());
  }
  return ret / 1024.0 / 1024.0; // Convert to MB
}

} // namespace lotus::cfl::cs_index::flare::tabulation
