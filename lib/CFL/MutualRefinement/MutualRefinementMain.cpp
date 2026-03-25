#include "CFL/MutualRefinement/Grammar.h"
#include "CFL/MutualRefinement/Graph.h"
#include "CFL/MutualRefinement/Hasher.h"
#include <cassert>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

std::unordered_set<std::pair<int, int>, IntPairHasher> intersectResults(
    const std::vector<std::unordered_set<Edge, EdgeHasher>> &results) {
  size_t n = results.size();
  assert(n >= 1);
  std::unordered_set<std::pair<int, int>, IntPairHasher> pairset;
  for (auto &e : results[0]) {
    if (std::get<0>(e) != std::get<2>(e)) {
      pairset.insert(std::make_pair(std::get<0>(e), std::get<2>(e)));
    }
  }
  for (size_t i = 1; i < n; i++) {
    std::unordered_set<std::pair<int, int>, IntPairHasher> tmp;
    for (auto &e : results[i]) {
      std::pair<int, int> p = std::make_pair(std::get<0>(e), std::get<2>(e));
      if (pairset.count(p) > 0) {
        tmp.insert(p);
      }
    }
    pairset = std::move(tmp);
  }
  return pairset;
}

std::unordered_map<std::string, int>
number(const std::vector<std::string> &names) {
  std::unordered_map<std::string, int> mp;
  size_t n = 0;
  for (auto &name : names) {
    if (mp.count(name) == 0) {
      mp[name] = n++;
    }
  }
  return mp;
}

template <typename T, typename U>
std::unordered_map<U, T> reverseMap(const std::unordered_map<T, U> &mp) {
  std::unordered_map<U, T> mpR;
  for (auto &kv : mp) {
    mpR[kv.second] = kv.first;
  }
  return mpR;
}

/* Raw grammars and raw graphs are ones with original strings
 * as symbols and node names. After encoding those strings as integers,
 * we get Grammar and Graph objects to use in CFL-reachabilty. */

std::vector<std::vector<std::vector<std::string>>>
readRawGrammars(const std::string &file) {
  std::ifstream in(file);
  std::vector<std::vector<std::vector<std::string>>> rawGrammars;
  std::vector<std::vector<std::string>> rawGrammar;
  std::string line;
  while (getline(in, line)) {
    if (line.size() > 0) {
      if (line[0] == '{') {
        rawGrammar = std::vector<std::vector<std::string>>();
      } else if (line[0] == '|') {
        std::istringstream sin(line.substr(1, line.size() - 1));
        std::vector<std::string> items;
        std::string item;
        while (sin >> item) {
          items.push_back(std::move(item));
        }
        rawGrammar.push_back(std::move(items));
      } else if (line[0] == '}') {
        rawGrammars.push_back(std::move(rawGrammar));
      }
    }
  }
  return rawGrammars;
}

std::vector<std::tuple<std::string, std::string, std::string>>
readRawGraph(const std::string &file) {
  // This stream will automatically be closed via RAII.
  std::ifstream in(file);
  std::vector<std::tuple<std::string, std::string, std::string>> rawEdges;
  std::string line;
  while (getline(in, line)) {
    // node1->node2[label="label"]
    if (line.find("->") != std::string::npos) {
      std::string::size_type p1 = line.find("->");
      std::string::size_type p2 = line.find('[');
      std::string::size_type p3 = line.find('=');
      std::string::size_type p4 = line.find(']');
      rawEdges.push_back(std::make_tuple(
          line.substr(0, p1 - 0), line.substr(p3 + 2, (p4 - 1) - (p3 + 2)),
          line.substr(p1 + 2, p2 - (p1 + 2))));
    }
  }
  return rawEdges;
}

std::tuple<std::vector<Grammar>, int, std::unordered_set<Edge, EdgeHasher>>
encode(
    std::vector<std::vector<std::vector<std::string>>> rawGrammars,
    std::vector<std::tuple<std::string, std::string, std::string>> rawGraph) {
  // Edge symbol encoding
  std::vector<std::string> symbols;
  for (auto &rawGrammar : rawGrammars) {
    for (auto &line : rawGrammar) {
      for (auto &sym : line) {
        symbols.push_back(sym);
      }
    }
  }
  std::unordered_map<std::string, int> symMap = number(symbols);
  std::unordered_map<int, std::string> symMapR = reverseMap(symMap);
  // Node encoding
  std::vector<std::string> nodes;
  for (auto &rawEdge : rawGraph) {
    nodes.push_back(std::get<0>(rawEdge));
    nodes.push_back(std::get<2>(rawEdge));
  }
  std::unordered_map<std::string, int> nodeMap = number(nodes);
  std::unordered_map<int, std::string> nodeMapR = reverseMap(nodeMap);
  // Grammar construction
  std::vector<Grammar> grammars;
  for (auto &rawGrammar : rawGrammars) {
    Grammar gm;
    // Symbols
    for (auto &line : rawGrammar) {
      for (auto &sym : line) {
        if (isupper(sym[0])) {
          gm.addNonterminal(symMap[sym]);
        } else {
          assert(islower(sym[0]));
          gm.addTerminal(symMap[sym]);
        }
      }
    }
    // Productions
    size_t n = rawGrammar.size();
    for (size_t i = 1; i < n; i++) {
      if (rawGrammar[i].size() == 1) {
        gm.addEmptyProduction(symMap[rawGrammar[i][0]]);
      } else if (rawGrammar[i].size() == 2) {
        gm.addUnaryProduction(symMap[rawGrammar[i][0]],
                              symMap[rawGrammar[i][1]]);
      } else {
        assert(rawGrammar[i].size() == 3);
        gm.addBinaryProduction(symMap[rawGrammar[i][0]],
                               symMap[rawGrammar[i][1]],
                               symMap[rawGrammar[i][2]]);
      }
    }
    // Start symbol
    gm.addStartSymbol(symMap[rawGrammar[0][0]]);
    // Initialize
    gm.initFastIndices();
    grammars.push_back(std::move(gm));
  }
  // Graph node count
  size_t numNode = nodeMap.size();
  // Graph edges
  std::unordered_set<Edge, EdgeHasher> edges;
  for (auto &rawEdge : rawGraph) {
    edges.insert(std::make_tuple(nodeMap[std::get<0>(rawEdge)],
                                 symMap[std::get<1>(rawEdge)],
                                 nodeMap[std::get<2>(rawEdge)]));
  }
  return std::make_tuple(std::move(grammars), numNode, std::move(edges));
}

void run(int argc, char *argv[]) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " <grammar-file> <graph-file> <\"naive\"/\"refine\">"
              << '\n';
    return;
  }
  // Get arguments
  std::string grammarFile = argv[1];
  std::string graphFile = argv[2];
  std::string mode = argv[3];
  // Read and encode
  auto encoded = encode(readRawGrammars(grammarFile), readRawGraph(graphFile));
  const std::vector<Grammar> &grammars = std::get<0>(encoded);
  size_t numNode = std::get<1>(encoded);
  const std::unordered_set<Edge, EdgeHasher> &edges = std::get<2>(encoded);
  size_t numGrammar = grammars.size();
  // Handle modes
  if (mode == "naive") {
    std::vector<Graph> graphs(numGrammar);
    std::vector<std::unordered_set<Edge, EdgeHasher>> results(numGrammar);
    for (size_t i = 0; i < numGrammar; i++) {
      graphs[i].reinit(numNode, edges);
      results[i] = graphs[i].runCFLReachability(grammars[i]);
    }
    std::cout << "Number of Reachable Pairs (Excluding Self-loops): "
              << intersectResults(results).size() << '\n';
  } else if (mode == "refine") {
    std::unordered_set<Edge, EdgeHasher> edgeSet = edges;
    size_t originalEdgeSetSize = edgeSet.size();
    std::unordered_set<Edge, EdgeHasher>::size_type previousEdgeSetSize;
    std::vector<Graph> graphs(numGrammar);
    std::vector<std::unordered_set<Edge, EdgeHasher>> results(numGrammar);
    size_t refineIterationCounter = 0;
    // Mutual refinement loop
    do {
      previousEdgeSetSize = edgeSet.size();
      for (size_t i = 0; i < numGrammar; i++) {
        graphs[i].reinit(numNode, edgeSet);
        std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>
            singleRecord;
        std::unordered_map<
            Edge,
            std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
            EdgeHasher>
            binaryRecord;
        results[i] = graphs[i].runCFLReachability(grammars[i], singleRecord,
                                                  binaryRecord);
        edgeSet = graphs[i].getEdgeClosure(grammars[i], results[i],
                                           singleRecord, binaryRecord);
      }
      refineIterationCounter++;
    } while (edgeSet.size() != previousEdgeSetSize);
    size_t reducedEdgeSetSize = edgeSet.size();
    std::cout << "Edge Set Reduction: " << originalEdgeSetSize << " -> "
              << reducedEdgeSetSize << '\n';
    std::cout << "Number of Refinement Iterations: " << refineIterationCounter
              << '\n';
    std::cout << "Number of Reachable Pairs (Excluding Self-loops): "
              << intersectResults(results).size() << '\n';
  }
}

std::string getPeakMemory() {
  std::ifstream in("/proc/self/status");
  std::string line;
  std::string vmpeak;
  while (getline(in, line)) {
    std::istringstream sin(line);
    std::string tag;
    sin >> tag;
    if (tag == "VmPeak:") {
      sin >> vmpeak;
      break;
    }
  }
  return vmpeak;
}

int mutual_refinement_main(int argc, char *argv[]) {
  auto start = std::chrono::steady_clock::now();
  run(argc, argv);
  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> elapsed_seconds = end - start;
  std::cout << "*** Resource Consumption ***" << '\n'
            << "Total Time (Seconds): " << elapsed_seconds.count() << '\n'
            << "Peak Space (kB): " << getPeakMemory() << '\n';
  return 0;
}
