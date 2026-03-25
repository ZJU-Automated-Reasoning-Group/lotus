#pragma once

#include <vector>

#include <llvm/ADT/SmallPtrSet.h>

namespace tpa {

class CFG;
class CFGNode;

// Fix #6: PriorityAssigner now assigns Reverse Post-Order (RPO) labels instead
// of post-order labels. RPO ensures that for a forward data-flow analysis the
// worklist processes nodes in the correct order (predecessors before successors
// in the acyclic case), minimising the number of fixpoint iterations needed.
//
// The `currLabel` member is kept for ABI compatibility but is no longer used
// by the implementation (labels are assigned in a second pass after the DFS).
class PriorityAssigner {
private:
  CFG &cfg;
  size_t currLabel; // kept for ABI compatibility; unused by new implementation

  using NodeSet = llvm::SmallPtrSet<const CFGNode *, 32>;
  NodeSet visitedNodes;

  // DFS helper: visits node and all successors, appending to postOrder on
  // the way back up (post-order traversal).
  void visitNode(CFGNode *, std::vector<CFGNode *> &postOrder);

public:
  PriorityAssigner(CFG &c) : cfg(c), currLabel(1) {}

  void traverse();
};

} // namespace tpa
