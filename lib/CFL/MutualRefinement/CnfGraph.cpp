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
  return runCFLReachabilityCore(grammar, nullptr, nullptr);
}

std::unordered_set<Edge, EdgeHasher> CnfGraph::runCFLReachability(
    const CnfGrammar &grammar,
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher> &singleRecord,
    std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> &binaryRecord) {
  return runCFLReachabilityCore(grammar, &singleRecord, &binaryRecord);
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

std::unordered_set<Edge, EdgeHasher> CnfGraph::getFactorizedEdgeClosure(
    const CnfGrammar &grammar,
    const std::unordered_set<Edge, EdgeHasher> &result) const {
  // Build symbol-specific views of the already saturated closure. These are
  // the factorized Out_X(i) and In_X(j) relations, not derivation records.
  std::unordered_map<std::pair<int, int>, std::vector<int>, IntPairHasher>
      outgoing;
  std::unordered_map<std::pair<int, int>, std::vector<int>, IntPairHasher>
      incoming;
  for (const Edge &edge : fastEdgeTest) {
    const int source = std::get<0>(edge);
    const int symbol = std::get<1>(edge);
    const int target = std::get<2>(edge);
    outgoing[std::make_pair(symbol, source)].push_back(target);
    incoming[std::make_pair(symbol, target)].push_back(source);
  }

  std::unordered_set<Edge, EdgeHasher> closure;
  std::unordered_set<Edge, EdgeHasher> visited;
  std::deque<Edge> worklist;

  auto enqueue = [&visited, &worklist](const Edge &edge) {
    if (visited.insert(edge).second) {
      worklist.push_back(edge);
    }
  };
  for (const Edge &edge : result) {
    enqueue(edge);
  }

  while (!worklist.empty()) {
    const Edge edge = worklist.front();
    worklist.pop_front();
    const int source = std::get<0>(edge);
    const int symbol = std::get<1>(edge);
    const int target = std::get<2>(edge);

    if (grammar.terminals.count(symbol) != 0U) {
      closure.insert(edge);
      continue;
    }

    const auto unary = grammar.unaryL.find(symbol);
    if (unary != grammar.unaryL.end()) {
      for (int production_index : unary->second) {
        const int child = grammar.unaryProductions[production_index].second;
        const Edge child_edge = std::make_tuple(source, child, target);
        if (hasEdge(child_edge)) {
          enqueue(child_edge);
        }
      }
    }

    const auto binary = grammar.binaryL.find(symbol);
    if (binary == grammar.binaryL.end()) {
      continue;
    }
    for (int production_index : binary->second) {
      const auto &production = grammar.binaryProductions[production_index];
      const int left = production.second.first;
      const int right = production.second.second;
      const auto out = outgoing.find(std::make_pair(left, source));
      const auto in = incoming.find(std::make_pair(right, target));
      if (out == outgoing.end() || in == incoming.end()) {
        continue;
      }

      if (out->second.size() <= in->second.size()) {
        for (int pivot : out->second) {
          if (!hasEdge(std::make_tuple(pivot, right, target))) {
            continue;
          }
          enqueue(std::make_tuple(source, left, pivot));
          enqueue(std::make_tuple(pivot, right, target));
        }
      } else {
        for (int pivot : in->second) {
          if (!hasEdge(std::make_tuple(source, left, pivot))) {
            continue;
          }
          enqueue(std::make_tuple(source, left, pivot));
          enqueue(std::make_tuple(pivot, right, target));
        }
      }
    }
  }
  return closure;
}

/* The CFL-reachability algorithm */
std::unordered_set<Edge, EdgeHasher> CnfGraph::runCFLReachabilityCore(
    const CnfGrammar &grammar,
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher> *singleRecord,
    std::unordered_map<
        Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
        EdgeHasher> *binaryRecord) {
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
        if (singleRecord != nullptr) {
          (*singleRecord)[e1].insert(y);
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
          if (binaryRecord != nullptr) {
            (*binaryRecord)[e1].insert(std::make_tuple(y, j, z));
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
          if (binaryRecord != nullptr) {
            (*binaryRecord)[e1].insert(std::make_tuple(z, i, y));
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
