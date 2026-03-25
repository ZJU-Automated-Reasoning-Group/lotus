// Implementation of PriorityAssigner.
//
// Assigns topological priorities to CFG nodes.
//
// Purpose:
// The worklist algorithm (in the Engine) uses priorities to determine the order
// of node processing. Processing nodes in Reverse Post-Order (RPO)
// significantly speeds up convergence for forward data-flow analysis: in RPO, a
// node is always processed before its successors (in the acyclic case), so
// information flows forward in a single pass rather than requiring many
// iterations.
//
// Fix #6: The previous implementation assigned post-order labels (leaf nodes
// get low numbers, entry gets a high number). If the worklist is a min-heap
// (processes lowest priority first), this is backwards for a forward analysis:
// the entry node would be processed last, requiring O(n) extra iterations to
// propagate information from entry to leaves.
//
// Correct RPO assignment: perform a DFS and assign labels in *reverse*
// post-order. The entry node gets label 1 (processed first), and leaf nodes
// get higher labels (processed later). This matches the standard RPO
// convention used by LLVM's ReversePostOrderTraversal.
//
// Algorithm:
// 1. DFS from the entry node, recording nodes in post-order (push on finish).
// 2. Reverse the post-order list to get RPO.
// 3. Assign labels 1, 2, 3, ... in RPO order.

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/PriorityAssigner.h"

#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"

#include <vector>

namespace tpa {

void PriorityAssigner::traverse() {
  // Step 1: Collect nodes in post-order via DFS.
  std::vector<tpa::CFGNode *> postOrder;
  postOrder.reserve(64);

  for (auto *node : cfg) {
    if (node->getPriority() == 0u)
      visitNode(node, postOrder);
  }

  // Step 2: Assign RPO labels (reverse of post-order).
  // postOrder[0] is the last node finished (deepest leaf), so it gets the
  // highest RPO label. postOrder.back() is the first node finished (entry or
  // near-entry), so it gets label 1.
  unsigned label = 1;
  for (auto it = postOrder.rbegin(); it != postOrder.rend(); ++it) {
    (*it)->setPriority(label);
    ++label;
  }
}

void PriorityAssigner::visitNode(tpa::CFGNode *node,
                                 std::vector<tpa::CFGNode *> &postOrder) {
  if (!visitedNodes.insert(node).second)
    return;

  // Visit all successors first (DFS).
  for (auto const &succ : node->succs())
    visitNode(succ, postOrder);

  // Record in post-order on the way back up.
  postOrder.push_back(node);
}

} // namespace tpa
