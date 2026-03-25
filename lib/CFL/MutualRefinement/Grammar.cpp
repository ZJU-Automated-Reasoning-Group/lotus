#include "CFL/MutualRefinement/Grammar.h"
#include "CFL/MutualRefinement/Hasher.h"
#include <utility>

void Grammar::addTerminal(int t) { terminals.insert(t); }

void Grammar::addNonterminal(int nt) { nonterminals.insert(nt); }

void Grammar::addStartSymbol(int s) { startSymbol = s; }

void Grammar::addEmptyProduction(int l) { emptyProductions.push_back(l); }

void Grammar::addUnaryProduction(int l, int r) {
  unaryProductions.push_back(std::make_pair(l, r));
}

void Grammar::addBinaryProduction(int l, int r1, int r2) {
  binaryProductions.push_back(std::make_pair(l, std::make_pair(r1, r2)));
}

void Grammar::initFastIndices() {
  size_t ne = emptyProductions.size();
  for (size_t i = 0; i < ne; i++) {
    emptyL[emptyProductions[i]].push_back(i);
  }
  size_t nu = unaryProductions.size();
  for (size_t i = 0; i < nu; i++) {
    // first and second have different types here
    unaryL[unaryProductions[i].first].push_back(i);
    unaryR[unaryProductions[i].second].push_back(i);
  }
  size_t nb = binaryProductions.size();
  for (size_t i = 0; i < nb; i++) {
    binaryL[binaryProductions[i].first].push_back(i);
    binaryR[binaryProductions[i].second].push_back(i);
  }
}
