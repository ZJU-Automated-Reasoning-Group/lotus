/*
 * Shared IFDS/IDE Run State
 *
 * Reusable edge stores with monotonic insertion semantics used by tabulation
 * solvers to guarantee "no duplicate edge insertion" invariants.
 */

#pragma once

#include "Dataflow/IFDS/Core/IFDSFramework.h"

#include <set>
#include <unordered_set>
#include <vector>

namespace ifds {

template <typename Fact> struct IFDSRunState {
  using PathEdgeType = PathEdge<Fact>;
  using SummaryEdgeType = SummaryEdge<Fact>;

  std::unordered_set<PathEdgeType, PathEdgeHash<Fact>> path_edges;
  std::set<SummaryEdgeType> summary_edges;
  std::vector<PathEdgeType> worklist;

  void clear() {
    path_edges.clear();
    summary_edges.clear();
    worklist.clear();
  }

  bool add_path_edge(const PathEdgeType &edge) {
    if (!path_edges.insert(edge).second) {
      return false;
    }
    worklist.push_back(edge);
    return true;
  }

  bool add_summary_edge(const SummaryEdgeType &edge) {
    return summary_edges.insert(edge).second;
  }
};

} // namespace ifds

