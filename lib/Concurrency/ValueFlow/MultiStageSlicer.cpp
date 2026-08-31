#include "Concurrency/ValueFlow/MultiStageSlicer.h"

#include "Concurrency/Thread/ThreadCreationTree.h"

#include <queue>

#include <llvm/IR/InstIterator.h>

namespace lotus::analysis {

MultiStageSlicer::NodeSet MultiStageSlicer::collectCandidateRoots() const {
  NodeSet roots;
  for (const auto &entry : *graph_) {
    const SVFGNode *node = entry.second;
    if (!node)
      continue;
    for (const SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || !edge->isThreadMHPEdge())
        continue;
      roots.insert(edge->getSrcNode());
      roots.insert(edge->getDstNode());
    }
  }
  return roots;
}

MultiStageSlicer::NodeSet
MultiStageSlicer::backwardClosure(const NodeSet &roots, const NodeSet *allowed,
                                  bool includeThreadEdges) const {
  NodeSet result;
  std::queue<const SVFGNode *> worklist;
  for (const SVFGNode *root : roots) {
    if (root && (!allowed || allowed->count(root) != 0) &&
        result.insert(root).second)
      worklist.push(root);
  }

  while (!worklist.empty()) {
    const SVFGNode *node = worklist.front();
    worklist.pop();
    for (const SVFGEdge *edge : node->getInEdges()) {
      if (!edge || (!includeThreadEdges && edge->isThreadMHPEdge()))
        continue;
      const SVFGNode *predecessor = edge->getSrcNode();
      if (!predecessor || (allowed && allowed->count(predecessor) == 0))
        continue;
      if (result.insert(predecessor).second)
        worklist.push(predecessor);
    }
  }
  return result;
}

MultiStageSlicer::NodeSet
MultiStageSlicer::expandSynchronizationAndCallDependence(NodeSet nodes) {
  std::unordered_set<const llvm::Function *> functions;
  for (const SVFGNode *node : nodes)
    if (const llvm::Function *function = node->getFunction())
      functions.insert(function);

  bool changed = true;
  while (changed) {
    changed = false;
    if (threadTree_) {
      for (const ThreadCreationTree::ForkRelation &relation :
           threadTree_->forkRelations()) {
        if (!relation.site || !relation.target ||
            functions.count(relation.target) == 0)
          continue;
        if (functions.insert(relation.site->getFunction()).second)
          changed = true;
      }
    }
    for (const auto &entry : *graph_) {
      const SVFGNode *node = entry.second;
      if (!node)
        continue;
      const llvm::Function *function = node->getFunction();
      if (function && functions.count(function) != 0)
        changed |= nodes.insert(node).second;

      for (const SVFGEdge *edge : node->getOutEdges()) {
        if (!edge || (!edge->isCallEdge() && !edge->isRetEdge()))
          continue;
        const SVFGNode *destination = edge->getDstNode();
        const llvm::Function *sourceFunction = node->getFunction();
        const llvm::Function *destinationFunction =
            destination ? destination->getFunction() : nullptr;
        if (destinationFunction && sourceFunction &&
            functions.count(destinationFunction) != 0 &&
            functions.insert(sourceFunction).second) {
          changed = true;
        }
      }
    }
  }

  // Include every value-flow node in the selected caller/callee functions.
  for (const auto &entry : *graph_) {
    const SVFGNode *node = entry.second;
    if (node && node->getFunction() &&
        functions.count(node->getFunction()) != 0)
      nodes.insert(node);
  }
  for (const llvm::Function *function : functions)
    for (const llvm::Instruction &instruction : llvm::instructions(function))
      synchronizationInstructions_.insert(&instruction);
  return nodes;
}

std::unique_ptr<FilteredSVFGView> MultiStageSlicer::slice() {
  stats_ = {};
  synchronizationInstructions_.clear();
  stats_.originalNodes = graph_->getNumNodes();

  NodeSet roots = collectCandidateRoots();
  stats_.candidateRoots = roots.size();
  if (roots.empty()) {
    NodeSet all;
    for (const auto &entry : *graph_)
      if (entry.second)
        all.insert(entry.second);
    stats_.candidateNodes = all.size();
    stats_.synchronizationNodes = all.size();
    stats_.pointsToNodes = all.size();
    std::unordered_set<const llvm::Function *> functions;
    for (const SVFGNode *node : all)
      if (node->getFunction())
        functions.insert(node->getFunction());
    for (const llvm::Function *function : functions)
      for (const llvm::Instruction &instruction : llvm::instructions(function))
        synchronizationInstructions_.insert(&instruction);
    return std::make_unique<FilteredSVFGView>(*graph_, std::move(all));
  }

  NodeSet candidates = backwardClosure(roots, nullptr, false);
  stats_.candidateNodes = candidates.size();

  NodeSet synchronization =
      expandSynchronizationAndCallDependence(std::move(candidates));
  stats_.synchronizationNodes = synchronization.size();

  // Re-close from the alarm endpoints with the synchronization/call slice as
  // an upper bound, this time retaining thread-value-flow dependencies.
  NodeSet pointsTo = backwardClosure(roots, &synchronization, true);
  stats_.pointsToNodes = pointsTo.size();
  return std::make_unique<FilteredSVFGView>(*graph_, std::move(pointsTo));
}

} // namespace lotus::analysis
