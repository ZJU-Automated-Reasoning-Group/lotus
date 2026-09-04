#pragma once

#include "CFL/InterleavedDyck/MutualRefinement/CnfGrammar.h"
#include "CFL/InterleavedDyck/MutualRefinement/Hasher.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::interleaved_dyck::mutual_refinement {

/* Graph on which CFL-reachability is run
 * The type Edge is an alias for std::tuple<int, int, int>,
 * which uses (i, A, j) to represent the edge i --A--> j */
struct CnfGraph {
  void reinit(int n, const std::unordered_set<Edge, EdgeHasher> &edges);
  void addEdge(const Edge &e);
  bool hasEdge(const Edge &e) const;
  // Normal CFL-reachability (returns the set of S edges)
  std::unordered_set<Edge, EdgeHasher>
  runCFLReachability(const CnfGrammar &grammar);
  // CFL-reachability with tracing for mutual refinement (returns the set of S
  // edges)
  std::unordered_set<Edge, EdgeHasher> runCFLReachability(
      const CnfGrammar &grammar,
      std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>
          &singleRecord,
      std::unordered_map<
          Edge,
          // For i --A--> j --B--> k, the triple is (A, j, B)
          std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
          EdgeHasher> &binaryRecord);
  // Get original edges contributing to S edges
  std::unordered_set<Edge, EdgeHasher> getEdgeClosure(
      const CnfGrammar &grammar,
      const std::unordered_set<Edge, EdgeHasher> &result,
      const std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>
          &singleRecord,
      const std::unordered_map<
          Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
          EdgeHasher> &binaryRecord) const;
  // Reconstruct contributing edges directly from the saturated relations.
  // runCFLReachability(grammar) must be called before this method.
  std::unordered_set<Edge, EdgeHasher> getFactorizedEdgeClosure(
      const CnfGrammar &grammar,
      const std::unordered_set<Edge, EdgeHasher> &result) const;

private:
  std::unordered_set<Edge, EdgeHasher> fastEdgeTest;
  std::vector<std::vector<std::pair<int, int>>>
      adjacencyVector; // v1 -> [(sym, v2)]
  std::vector<std::vector<std::pair<int, int>>>
      counterAdjacencyVector; // v2 -> [(v1, sym)]
  // This function is called by the previous two overloaded runCFLReachability
  // functions
  std::unordered_set<Edge, EdgeHasher> runCFLReachabilityCore(
      const CnfGrammar &grammar,
      std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>
          *singleRecord,
      std::unordered_map<
          Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
          EdgeHasher> *binaryRecord);
};

} // namespace lotus::cfl::interleaved_dyck::mutual_refinement
