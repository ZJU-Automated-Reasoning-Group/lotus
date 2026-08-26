//===- SCD.cpp - Standard control dependence -----------------------------===//

#include "Analysis/ControlDependence/SCD.h"

#include "llvm/Analysis/DominanceFrontier.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Function.h"

namespace lotus::cd::detail {
namespace {

/// Post-dominance-frontier computation used by dg's LLVM SCD adapter.
class PostDominanceFrontiers {
  using FrontierBase = llvm::DominanceFrontierBase<llvm::BasicBlock, true>;
  using FrontierMap = FrontierBase::DomSetMapType;
  using FrontierSet = FrontierBase::DomSetType;

public:
  FrontierSet &calculate(const llvm::PostDominatorTree &tree,
                         const llvm::DomTreeNode *node) {
    llvm::BasicBlock *block = node->getBlock();
    FrontierSet &frontier = m_frontiers[block];
    if (tree.root_size() == 0)
      return frontier;

    if (block) {
      for (llvm::BasicBlock *predecessor : llvm::predecessors(block)) {
        auto *predecessorNode = tree[predecessor];
        if (predecessorNode && predecessorNode->getIDom() != node)
          frontier.insert(predecessor);
      }
    }

    for (const llvm::DomTreeNode *child : *node) {
      const FrontierSet &childFrontier = calculate(tree, child);
      for (llvm::BasicBlock *member : childFrontier)
        if (!tree.properlyDominates(node, tree[member]))
          frontier.insert(member);
    }
    return frontier;
  }

private:
  FrontierMap m_frontiers;
};

} // namespace

DependenceResult computeSCD(
    llvm::Function &function,
    const llvm::DenseMap<const llvm::BasicBlock *, GraphNode *> &blockToNode) {
  llvm::PostDominatorTree postDominators(function);
  DependenceMap dependencies;
  DependenceMap dependents;

  PostDominanceFrontiers frontiers;
  for (llvm::BasicBlock &dependent : function) {
    auto *dependentNode = postDominators.getNode(&dependent);
    if (!dependentNode)
      continue;

    for (llvm::BasicBlock *predicate :
         frontiers.calculate(postDominators, dependentNode)) {
      GraphNode *dependentGraphNode = blockToNode.lookup(&dependent);
      GraphNode *predicateGraphNode = blockToNode.lookup(predicate);
      dependencies[dependentGraphNode].insert(predicateGraphNode);
      dependents[predicateGraphNode].insert(dependentGraphNode);
    }
  }
  return {std::move(dependencies), std::move(dependents)};
}

} // namespace lotus::cd::detail
