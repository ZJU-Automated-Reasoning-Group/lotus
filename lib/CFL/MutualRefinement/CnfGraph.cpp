#include "CFL/MutualRefinement/CnfGraph.h"

#include "CFL/MutualRefinement/CnfGrammar.h"
#include "CFL/MutualRefinement/Hasher.h"

#include <cstddef>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lotus::cfl::mutual_refinement {

void CnfGraph::reinit(int n,
                      const std::unordered_set<Edge, EdgeHasher> &edges) {
  fastEdgeTest.clear();
  adjacencyVector.clear();
  adjacencyVector.resize(n);
  counterAdjacencyVector.clear();
  counterAdjacencyVector.resize(n);
  for (const Edge &e : edges) {
    addEdge(e);
  }
}

void CnfGraph::addEdge(const Edge &e) {
  fastEdgeTest.insert(e);
  adjacencyVector[std::get<0>(e)].push_back(
      std::make_pair(std::get<1>(e), std::get<2>(e)));
  counterAdjacencyVector[std::get<2>(e)].push_back(
      std::make_pair(std::get<0>(e), std::get<1>(e)));
}

bool CnfGraph::hasEdge(const Edge &e) const {
  return fastEdgeTest.count(e) == 1;
}

std::unordered_set<Edge, EdgeHasher>
CnfGraph::runCFLReachability(const CnfGrammar &grammar) {
  // To call runCFLReachabilityCore, those two maps are always needed,
  // but we can use the Boolean flag to enable or disable tracing
  std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher> singleRecord;
  std::unordered_map<
      Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
      EdgeHasher>
      binaryRecord;
  return runCFLReachabilityCore(grammar, false, singleRecord, binaryRecord);
}

std::unordered_set<Edge, EdgeHasher> CnfGraph::runCFLReachability(
    const CnfGrammar &grammar,
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher> &singleRecord,
    std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> &binaryRecord) {
  return runCFLReachabilityCore(grammar, true, singleRecord, binaryRecord);
}

std::unordered_set<Edge, EdgeHasher> CnfGraph::getEdgeClosure(
    const CnfGrammar &grammar,
    const std::unordered_set<Edge, EdgeHasher> &result,
    const std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>
        &singleRecord,
    const std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> &binaryRecord) const {
  // The set of edges to be returned, which only contains original edges in the
  // graph
  std::unordered_set<Edge, EdgeHasher> closure;
  // The set to avoid visiting the same edge (including summary edges)  more
  // than once
  std::unordered_set<Edge, EdgeHasher> vis;
  std::deque<Edge> w;
  // Start from all S edges
  for (const Edge &e : result) {
    vis.insert(e);
    w.push_back(e);
  }
  while (!w.empty()) {
    Edge e = w.front();
    w.pop_front();
    // i --x--> j
    int i = std::get<0>(e);
    int x = std::get<1>(e);
    int j = std::get<2>(e);
    if (grammar.terminals.count(x) == 1) {
      // Only insert terminal (original) edges
      closure.insert(e);
    } else {
      // This is a const member function so we can't write singleRecord[e].
      if (singleRecord.count(e) == 1) {
        for (int y : singleRecord.at(e)) {
          Edge e1 = std::make_tuple(i, y, j);
          if (vis.count(e1) == 0) {
            vis.insert(e1);
            w.push_back(e1);
          }
        }
      }
      if (binaryRecord.count(e) == 1) {
        for (const auto &triple : binaryRecord.at(e)) {
          int y = std::get<0>(triple);
          int k = std::get<1>(triple);
          int z = std::get<2>(triple);
          Edge e1 = std::make_tuple(i, y, k);
          Edge e2 = std::make_tuple(k, z, j);
          if (vis.count(e1) == 0) {
            vis.insert(e1);
            w.push_back(e1);
          }
          if (vis.count(e2) == 0) {
            vis.insert(e2);
            w.push_back(e2);
          }
        }
      }
    }
  }
  return closure;
}

/* The CFL-reachability algorithm */
std::unordered_set<Edge, EdgeHasher> CnfGraph::runCFLReachabilityCore(
    const CnfGrammar &grammar, const bool record,
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher> &singleRecord,
    std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> &binaryRecord) {
  std::unordered_set<Edge, EdgeHasher> result;
  std::deque<Edge> w;
  // Original edges
  for (const Edge &e : fastEdgeTest) {
    w.push_front(e);
  }
  // Empty productions
  size_t nv = adjacencyVector.size();
  for (size_t x : grammar.emptyProductions) {
    for (size_t i = 0; i < nv; i++) {
      Edge e = std::make_tuple(i, x, i);
      addEdge(e);
      w.push_front(e);
      if (static_cast<size_t>(grammar.startSymbol) == x) {
        result.insert(e);
      }
    }
  }
  // Other productions
  while (!w.empty()) {
    Edge e = w.front();
    w.pop_front();

    // i --y--> j
    int i = std::get<0>(e);
    int y = std::get<1>(e);
    int j = std::get<2>(e);

    // Edges to be added
    std::vector<Edge> tba;

    if (grammar.unaryR.count(y) == 1) {
      for (int ind : grammar.unaryR.at(y)) {
        // x -> y
        int x = grammar.unaryProductions[ind].first;
        Edge e1 = std::make_tuple(i, x, j);
        tba.push_back(e1);
        // May use conditional compilation to avoid the if-test cost
        if (record) {
          singleRecord[e1].insert(y);
        }
      }
    }
    for (auto &zk : adjacencyVector[j]) {
      // x -> yz
      int z = zk.first;
      int k = zk.second;
      if (grammar.binaryR.count(std::make_pair(y, z)) == 1) {
        for (int ind : grammar.binaryR.at(std::make_pair(y, z))) {
          int x = grammar.binaryProductions[ind].first;
          Edge e1 = std::make_tuple(i, x, k);
          tba.push_back(e1);
          if (record) {
            binaryRecord[e1].insert(std::make_tuple(y, j, z));
          }
        }
      }
    }
    for (auto &kz : counterAdjacencyVector[i]) {
      // x -> zy
      int k = kz.first;
      int z = kz.second;
      if (grammar.binaryR.count(std::make_pair(z, y)) == 1) {
        for (int ind : grammar.binaryR.at(std::make_pair(z, y))) {
          int x = grammar.binaryProductions[ind].first;
          Edge e1 = std::make_tuple(k, x, j);
          tba.push_back(e1);
          if (record) {
            binaryRecord[e1].insert(std::make_tuple(z, i, y));
          }
        }
      }
    }
    for (Edge &e1 : tba) {
      if (!hasEdge(e1)) {
        addEdge(e1);
        w.push_front(e1);
        if (std::get<1>(e1) == grammar.startSymbol) {
          result.insert(e1);
        }
      }
    }
  }
  return result;
}

} // namespace lotus::cfl::mutual_refinement
