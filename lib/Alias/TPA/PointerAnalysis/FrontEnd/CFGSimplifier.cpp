// Implementation of CFGSimplifier.
//
// This class performs graph transformations to reduce the size and complexity
// of the CFG.
//
// Optimization: Redundant Node Elimination
// 1. Identify nodes that are "identity" transforms:
//    - Copy nodes with a single source (dst = src).
//    - Offset nodes with 0 offset (dst = src + 0).
// 2. These nodes can be removed, and all their uses replaced by their
// definitions.
//    (effectively merging the pointer equivalence classes).
// 3. Updates Def-Use chains to bypass the removed nodes.

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/CFGSimplifier.h"

#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/NodeVisitor.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>

using namespace llvm;

namespace tpa {

namespace {

// Visitor to rewrite node operands based on the equivalence map.
class CFGAdjuster : public NodeVisitor<CFGAdjuster> {
private:
  using MapType = DenseMap<const Value *, const Value *>;
  const MapType &eqMap;

  using SetType = util::VectorSet<CFGNode *>;
  const SetType &redundantSet;

  // Helper to find the replacement value for v (if any).
  const Value *lookup(const Value *v) {
    assert(v != nullptr);
    auto itr = eqMap.find(v);
    if (itr == eqMap.end())
      return nullptr;
    else
      return itr->second;
  }

public:
  CFGAdjuster(const MapType &m, const SetType &s) : eqMap(m), redundantSet(s) {}

  void visitEntryNode(EntryCFGNode &) {}
  void visitAllocNode(AllocCFGNode &) {}

  void visitCopyNode(CopyCFGNode &copyNode) {
    if (redundantSet.count(&copyNode))
      return;
    assert(!eqMap.count(copyNode.getDest()));

    std::vector<const Value *> newSrcs;
    newSrcs.reserve(copyNode.getNumSrc());
    SmallPtrSet<const Value *, 16> visitedSrcs;
    for (const auto *src : copyNode) {
      if (!visitedSrcs.insert(src).second)
        continue;
      // If source is replaced, use replacement
      if (const auto *newSrc = lookup(src))
        newSrcs.push_back(newSrc);
      else
        newSrcs.push_back(src);
    }

    copyNode.setSrc(std::move(newSrcs));
  }

  void visitOffsetNode(OffsetCFGNode &offsetNode) {
    if (redundantSet.count(&offsetNode))
      return;
    assert(!eqMap.count(offsetNode.getDest()));

    if (const auto *newSrc = lookup(offsetNode.getSrc()))
      offsetNode.setSrc(newSrc);
  }

  void visitLoadNode(LoadCFGNode &loadNode) {
    assert(!eqMap.count(loadNode.getDest()));
    if (const auto *newSrc = lookup(loadNode.getSrc()))
      loadNode.setSrc(newSrc);
  }

  void visitStoreNode(StoreCFGNode &storeNode) {
    if (const auto *newDest = lookup(storeNode.getDest()))
      storeNode.setDest(newDest);
    if (const auto *newSrc = lookup(storeNode.getSrc()))
      storeNode.setSrc(newSrc);
  }

  void visitCallNode(CallCFGNode &callNode) {
    assert(!eqMap.count(callNode.getDest()));
    if (const auto *newFunc = lookup(callNode.getFunctionPointer()))
      callNode.setFunctionPointer(newFunc);
    for (auto i = 0u, e = callNode.getNumArgument(); i < e; ++i) {
      if (const auto *newArg = lookup(callNode.getArgument(i)))
        callNode.setArgument(i, newArg);
    }
  }

  void visitReturnNode(ReturnCFGNode &retNode) {
    if (const auto *retVal = retNode.getReturnValue()) {
      if (const auto *newVal = lookup(retVal))
        retNode.setReturnValue(newVal);
    }
  }
};

} // namespace

// Scans CFG for redundant nodes.
// Populates eqMap with {Dest -> Src} mappings.
std::vector<CFGNode *> CFGSimplifier::findRedundantNodes(CFG &cfg) {
  std::vector<CFGNode *> ret;
  for (auto *node : cfg) {
    if (node->isCopyNode()) {
      auto *copyNode = static_cast<CopyCFGNode *>(node);
      // Copy with 1 source is an identity: Dest == Src
      if (copyNode->getNumSrc() == 1u) {
        ret.push_back(copyNode);
        eqMap[copyNode->getDest()] = copyNode->getSrc(0);
      }
    }

    if (node->isOffsetNode()) {
      auto *offsetNode = static_cast<OffsetCFGNode *>(node);
      // Offset 0 is an identity: Dest == Src
      if (offsetNode->getOffset() == 0u) {
        ret.push_back(offsetNode);
        eqMap[offsetNode->getDest()] = offsetNode->getSrc();
      }
    }
  }
  return ret;
}

// Flattens the equivalence map (path compression).
// If A->B and B->C, update A->C.
void CFGSimplifier::flattenEquivalentMap() {
  auto find = [this](const Value *val) {
    while (true) {
      auto itr = eqMap.find(val);
      if (itr == eqMap.end())
        break;
      else
        val = itr->second;
    }
    assert(val != nullptr);
    return val;
  };

  for (auto &mapping : eqMap)
    mapping.second = find(mapping.second);
}

// Updates all nodes in the CFG to use the representative values.
void CFGSimplifier::adjustCFG(
    CFG &cfg, const util::VectorSet<CFGNode *> &redundantNodes) {
  auto adjuster = CFGAdjuster(eqMap, redundantNodes);
  for (auto *node : cfg)
    adjuster.visit(*node);
}

// Rewires Def-Use edges to bypass redundant nodes.
void CFGSimplifier::adjustDefUseChain(
    CFG &cfg, const util::VectorSet<tpa::CFGNode *> &redundantNodes) {
  for (auto *node : redundantNodes) {
    // node may be assigned #null or #universal, in which cases the def size may
    // be 0 (no definition drives this node).
    if (node->def_size() > 0u) {
      assert(node->def_size() == 1u);
      auto *defNode = *node->def_begin();
      defNode->removeDefUseEdge(node);

      // Connect definition directly to all uses, bypassing the redundant node.
      for (auto *useNode : node->uses())
        defNode->insertDefUseEdge(useNode);
    }
    // Bug fix: when def_size() == 0, the redundant node has no definition
    // driving it (e.g., it was assigned null/universal). In this case we
    // cannot rewire the def-use edges to a parent def. The use nodes that
    // depended on this redundant node will lose their def-use connection.
    // To avoid leaving them completely disconnected (which would cause
    // premature fixpoints), we connect them to the CFG entry node, which
    // acts as the universal definition root for constants and externals.
    else if (node->use_size() > 0u) {
      auto *entryNode = cfg.getEntryNode();
      for (auto *useNode : node->uses())
        entryNode->insertDefUseEdge(useNode);
    }

    // Clean up node's edges (snapshot uses first since we're modifying the
    // list)
    auto uses = SmallVector<CFGNode *, 8>(node->use_begin(), node->use_end());
    for (auto *useNode : uses)
      node->removeDefUseEdge(useNode);

    assert(node->def_size() == 0u && node->use_size() == 0u);
  }
}

void CFGSimplifier::removeNodes(
    CFG &cfg, const util::VectorSet<CFGNode *> &redundantNodes) {
  cfg.removeNodes(redundantNodes);
}

// Main optimization loop.
// Runs iteratively until no more redundant nodes are found (fixpoint).
void CFGSimplifier::simplify(CFG &cfg) {
  while (true) {
    auto redundantNodes = util::VectorSet<CFGNode *>(findRedundantNodes(cfg));
    if (redundantNodes.empty())
      break;

    flattenEquivalentMap();
    adjustCFG(cfg, redundantNodes);
    adjustDefUseChain(cfg, redundantNodes);
    removeNodes(cfg, redundantNodes);

    eqMap.clear();
  }
}

} // namespace tpa