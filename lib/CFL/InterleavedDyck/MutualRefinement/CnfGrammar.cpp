#include "CFL/InterleavedDyck/MutualRefinement/CnfGrammar.h"

#include "CFL/InterleavedDyck/MutualRefinement/Hasher.h"

#include <utility>

namespace lotus::cfl::interleaved_dyck::mutual_refinement {

void CnfGrammar::addTerminal(int t) { terminals.insert(t); }

void CnfGrammar::addNonterminal(int nt) { nonterminals.insert(nt); }

void CnfGrammar::addStartSymbol(int s) { startSymbol = s; }

void CnfGrammar::addEmptyProduction(int l) { emptyProductions.push_back(l); }

void CnfGrammar::addUnaryProduction(int l, int r) {
  unaryProductions.push_back(std::make_pair(l, r));
}

void CnfGrammar::addBinaryProduction(int l, int r1, int r2) {
  binaryProductions.push_back(std::make_pair(l, std::make_pair(r1, r2)));
}

void CnfGrammar::initFastIndices() {
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

} // namespace lotus::cfl::interleaved_dyck::mutual_refinement
