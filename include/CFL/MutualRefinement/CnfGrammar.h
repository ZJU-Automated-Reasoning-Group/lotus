#pragma once

#include "CFL/MutualRefinement/Hasher.h"

#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::mutual_refinement {

/* Context-free grammar in Chomsky normal form
 * Symbols are encoded as integers */
struct CnfGrammar {
  /* Member functions to add symbols and productions */
  void addTerminal(int t);
  void addNonterminal(int nt);
  void addStartSymbol(int s);
  void addEmptyProduction(int l);
  void addUnaryProduction(int l, int r);
  void addBinaryProduction(int l, int r1, int r2);
  std::unordered_set<int> terminals;
  std::unordered_set<int> nonterminals;
  int startSymbol;
  std::vector<int> emptyProductions;
  std::vector<std::pair<int, int>> unaryProductions;
  std::vector<std::pair<int, std::pair<int, int>>> binaryProductions;
  /* Fast production index finding
   * (index refers to the production's index in the above vectors) */
  // To be called after adding all symbols and productions
  void initFastIndices();
  // symbol -> indices of empty productions whose LHS is the symbol
  std::unordered_map<int, std::vector<int>> emptyL;
  // similar
  std::unordered_map<int, std::vector<int>> unaryL;
  // symbol -> indices of unary productions whose RHS is the symbol
  std::unordered_map<int, std::vector<int>> unaryR;
  // similar
  std::unordered_map<int, std::vector<int>> binaryL;
  // similar
  std::unordered_map<std::pair<int, int>, std::vector<int>, IntPairHasher>
      binaryR;
};

} // namespace lotus::cfl::mutual_refinement
