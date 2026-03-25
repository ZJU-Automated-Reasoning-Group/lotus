#include "Analysis/CFG/TopologicalOrder.h"

#include "llvm/Analysis/CFG.h"

#include "Analysis/CFG/SortTopo.h"

#include <algorithm>

char TopologicalOrder::ID = 0;

void TopologicalOrder::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

bool TopologicalOrder::runOnFunction(Function &F) {
  m_backEdges.clear();
  m_order.clear();

  FindFunctionBackedges(F, m_backEdges);
  std::sort(m_backEdges.begin(), m_backEdges.end());

  RevTopoSort(F, m_order);
  std::reverse(m_order.begin(), m_order.end());

  return false;
}

bool TopologicalOrder::isBackEdge(const BasicBlock &src,
                                  const BasicBlock &dst) const {
  return std::binary_search(m_backEdges.begin(), m_backEdges.end(),
                            std::make_pair(&src, &dst));
}

void TopologicalOrder::print(raw_ostream &out, const Module *m) const {
  out << "TOPO BEGIN\n";

  for (auto *bb : *this)
    out << bb->getName() << " ";
  out << "\n";

  out << "TOPO END\n";
}

static llvm::RegisterPass<TopologicalOrder>
    X("topo", "Topological order of CFG", true, true);
