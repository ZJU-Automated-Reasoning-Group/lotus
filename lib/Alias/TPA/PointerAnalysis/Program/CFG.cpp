// Implementation of the Control Flow Graph (CFG) and CFGNodes.
//
// The TPA CFG is a "semi-sparse" representation optimized for pointer analysis.
// Unlike the standard LLVM CFG (BasicBlocks containing Instructions), the TPA
// CFG consists of `CFGNode`s that correspond only to pointer-relevant
// instructions.
//
// Structure:
// - CFG: Represents a function's control flow graph.
// - CFGNode: Abstract base class for nodes (Alloc, Copy, Store, etc.).
// - Edges:
//   - Control Flow (succ/pred): Standard execution order.
//   - Def-Use (def/use): Data flow dependencies for top-level pointers
//   (SSA-like).

#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"

#include "Alias/TPA/PointerAnalysis/Program/CFG/NodeVisitor.h"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>

using namespace llvm;

namespace tpa {

// --- CFGNode Implementation ---

const Function &CFGNode::getFunction() const {
  assert(cfg != nullptr);
  return cfg->getFunction();
}

// Manages control-flow edges.
void CFGNode::insertEdge(CFGNode *node) {
  assert(node != nullptr);

  succ.insert(node);
  (node->pred).insert(this);
}

void CFGNode::removeEdge(CFGNode *node) {
  assert(node != nullptr);

  succ.erase(node);
  (node->pred).erase(this);
}

// Manages data-flow (def-use) edges.
// Note: Def-Use edges bypass control flow for top-level pointer propagation.
void CFGNode::insertDefUseEdge(CFGNode *node) {
  assert(node != nullptr);

  use.insert(node);
  node->def.insert(this);
}

void CFGNode::removeDefUseEdge(CFGNode *node) {
  assert(node != nullptr);

  use.erase(node);
  node->def.erase(this);
}

// Removes a node from the graph, rewiring its predecessors to its successors.
// Used during CFG simplification (e.g., removing redundant nodes).
void CFGNode::detachFromCFG() {
  // Remove edges to predecessors and bypass this node
  auto preds = SmallVector<CFGNode *, 8>(pred.begin(), pred.end());
  for (auto *predNode : preds) {
    // Ignore self-loop
    if (predNode == this)
      continue;

    for (auto *succNode : succ) {
      // Again, ignore self-loop
      if (succNode == this)
        continue;

      // Connect pred directly to succ
      predNode->insertEdge(succNode);
    }

    // Disconnect pred from this
    predNode->removeEdge(this);
  }

  // Remove edges to successors
  auto succs = SmallVector<CFGNode *, 8>(succ.begin(), succ.end());
  for (auto *succNode : succs)
    removeEdge(succNode);
}

// --- CFG Implementation ---

CFG::CFG(const Function &f)
    : func(f), entryNode(create<EntryCFGNode>()), exitNode(nullptr) {
  entryNode->setCFG(*this);
}

// Bulk removal of nodes.
void CFG::removeNodes(const util::VectorSet<CFGNode *> &removeSet) {
  if (removeSet.empty())
    return;

  NodeList newNodeList;
  newNodeList.reserve(nodes.size());
  for (auto &node : nodes) {
    if (!removeSet.count(node.get()))
      newNodeList.emplace_back(std::move(node));
    else {
      node->detachFromCFG();
      assert(node->pred_size() == 0u && node->succ_size() == 0u &&
             node->def_size() == 0u && node->use_size() == 0u);
    }
  }
  nodes.swap(newNodeList);
}

namespace {

// Visitor to build a map from LLVM Values to CFG Nodes.
// This map allows looking up the "definition node" for any given pointer value.
class ValueMapVisitor : public ConstNodeVisitor<ValueMapVisitor> {
private:
  using MapType = DenseMap<const Value *, const CFGNode *>;
  MapType &valueMap;

public:
  ValueMapVisitor(MapType &m) : valueMap(m) {}

  void visitEntryNode(const EntryCFGNode &entryNode) {
    auto const &func = entryNode.getFunction();
    for (auto const &arg : func.args()) {
      valueMap[&arg] = &entryNode;
    }
  }
  void visitAllocNode(const AllocCFGNode &allocNode) {
    valueMap[allocNode.getDest()] = &allocNode;
  }
  void visitCopyNode(const CopyCFGNode &copyNode) {
    valueMap[copyNode.getDest()] = &copyNode;
  }
  void visitOffsetNode(const OffsetCFGNode &offsetNode) {
    valueMap[offsetNode.getDest()] = &offsetNode;
  }
  void visitLoadNode(const LoadCFGNode &loadNode) {
    valueMap[loadNode.getDest()] = &loadNode;
  }
  // Stores and Returns don't define new pointer values.
  void visitStoreNode(const StoreCFGNode &) {}
  void visitCallNode(const CallCFGNode &callNode) {
    if (const auto *dst = callNode.getDest())
      valueMap[dst] = &callNode;
  }
  void visitReturnNode(const ReturnCFGNode &) {}
};

} // namespace

// Rebuilds the Value -> Node mapping.
// Must be called after CFG simplification or modification.
void CFG::buildValueMap() {
  valueMap.clear();

  ValueMapVisitor visitor(valueMap);
  for (auto const &node : nodes)
    visitor.visit(*node);
}

} // namespace tpa