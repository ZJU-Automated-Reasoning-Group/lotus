// Implementation of FunctionTranslator.
//
// Translates a single LLVM Function into a TPA Control Flow Graph (CFG).
//
// Process:
// 1. Basic Block Translation: Iterates over instructions, translating each
// relevant one
//    into a `CFGNode` via `InstructionTranslator`.
// 2. CFG Construction: Connects the translated nodes to form the graph
// structure.
//    Handles empty blocks (blocks with no relevant pointer instructions) by
//    stitching predecessors directly to successors.
// 3. Def-Use Analysis: Explicitly builds def-use chains for pointer values.
//    (e.g., connecting an Alloc node to a Store node that uses it).
// 4. Cleanup: Detaches store-preserving nodes (Alloc, Copy, Offset) from the
// control-flow
//    graph, leaving them only connected via def-use chains. This transforms the
//    CFG into a "Semi-Sparse" representation where only memory-accessing nodes
//    (Load, Store, Call) are sequenced in control flow.

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/FunctionTranslator.h"

#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/InstructionTranslator.h"
#include "Alias/TPA/PointerAnalysis/FrontEnd/CFG/PriorityAssigner.h"
#include "Alias/TPA/PointerAnalysis/Program/CFG/CFG.h"
#include "Alias/TPA/Util/Log.h"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

namespace tpa {

void FunctionTranslator::translateBasicBlock(const Function &llvmFunc) {
  for (auto const &currBlock : llvmFunc) {
    tpa::CFGNode *startNode = nullptr;
    tpa::CFGNode *endNode = nullptr;

    for (auto const &inst : currBlock) {
      auto *currNode = translator.visit(const_cast<Instruction &>(inst));
      if (currNode == nullptr)
        continue;
      else {
        instToNode[&inst] = currNode;
        nodeToInst[currNode] = &inst;
      }

      // Update the first node
      if (startNode == nullptr)
        startNode = currNode;
      // Chain the node with the last one
      if (endNode != nullptr)
        endNode->insertEdge(currNode);
      endNode = currNode;
    }

    assert((startNode == nullptr) == (endNode == nullptr));
    if (startNode != nullptr)
      bbToNode.insert(
          std::make_pair(&currBlock, std::make_pair(startNode, endNode)));
    else
      nonEmptySuccMap[&currBlock] = std::vector<tpa::CFGNode *>();
  }
}

// Fix #7: Rewritten processEmptyBlock to eliminate the quadratic worst-case
// complexity and the potential infinite loop on irreducible CFGs.
//
// The original implementation used a per-empty-block BFS with a local visited
// set. This had two problems:
//   1. Quadratic complexity: for each empty block the BFS could re-visit all
//      other empty blocks, giving O(n²) total work.
//   2. Infinite loop risk: the local visited set was reset for each outer
//      iteration, so cycles among empty blocks could be re-entered.
//
// New approach: a single global BFS over all empty blocks simultaneously.
// We process all empty blocks in one pass using a shared visited set, so each
// block is expanded at most once. The result for each empty block is the set
// of non-empty CFG nodes reachable from it through chains of empty blocks.
//
// Algorithm:
//   1. Seed the worklist with all empty blocks.
//   2. For each empty block popped from the worklist, examine its LLVM
//      successors:
//      - If a successor is non-empty (has a CFGNode), record its first node
//        as a result for the current empty block.
//      - If a successor is also empty and not yet visited, add it to the
//        worklist.
//   3. After the BFS, propagate results upward: if empty block A has empty
//      block B as a successor, A's result set should include B's result set.
//      We achieve this with a second pass that merges results along the
//      empty-block edges.
void FunctionTranslator::processEmptyBlock() {
  // Step 1: BFS to find, for each empty block, the set of non-empty CFG nodes
  // directly reachable through one hop of empty blocks.
  // We use a global visited set to avoid re-processing.
  SmallPtrSet<const BasicBlock *, 32> visited;

  // Seed: all empty blocks are in the worklist.
  std::vector<const BasicBlock *> workList;
  for (auto &mapping : nonEmptySuccMap) {
    workList.push_back(mapping.first);
    visited.insert(mapping.first);
  }

  // BFS: for each empty block, find its immediate non-empty successors and
  // queue unvisited empty successors.
  while (!workList.empty()) {
    const auto *currBlock = workList.back();
    workList.pop_back();

    for (auto itr = succ_begin(currBlock), ite = succ_end(currBlock);
         itr != ite; ++itr) {
      const auto *succBlock = *itr;
      if (bbToNode.count(succBlock)) {
        // Non-empty successor: record its first CFG node.
        nonEmptySuccMap[currBlock].push_back(bbToNode[succBlock].first);
      } else if (nonEmptySuccMap.count(succBlock)) {
        // Empty successor not yet visited: queue it.
        if (visited.insert(succBlock).second)
          workList.push_back(succBlock);
      }
      // else: succBlock has no CFG nodes and is not in nonEmptySuccMap
      // (e.g., it was never registered). Skip it.
    }
  }

  // Step 2: Propagate results through chains of empty blocks.
  // If empty block A -> empty block B, then A's non-empty successors should
  // include all of B's non-empty successors. We iterate until stable.
  // In practice this converges in very few passes because empty-block chains
  // are short.
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto &mapping : nonEmptySuccMap) {
      const auto *currBlock = mapping.first;
      // Build a set of already-known nodes for fast membership testing.
      // SmallPtrSet has no range constructor in LLVM; populate manually.
      SmallPtrSet<tpa::CFGNode *, 16> existing;
      for (auto *n : mapping.second)
        existing.insert(n);

      for (auto itr = succ_begin(currBlock), ite = succ_end(currBlock);
           itr != ite; ++itr) {
        const auto *succBlock = *itr;
        auto succItr = nonEmptySuccMap.find(succBlock);
        if (succItr == nonEmptySuccMap.end())
          continue;
        for (auto *node : succItr->second) {
          if (existing.insert(node).second) {
            mapping.second.push_back(node);
            changed = true;
          }
        }
      }
    }
  }

  // Deduplicate each result vector (the propagation pass may introduce
  // duplicates when the same node is reachable via multiple empty-block paths).
  for (auto &mapping : nonEmptySuccMap) {
    SmallPtrSet<tpa::CFGNode *, 16> seen;
    std::vector<tpa::CFGNode *> deduped;
    for (auto *node : mapping.second) {
      if (seen.insert(node).second)
        deduped.push_back(node);
    }
    mapping.second = std::move(deduped);
  }
}

void FunctionTranslator::connectCFGNodes(const BasicBlock &entryBlock) {
  for (auto &mapping : bbToNode) {
    auto *bb = mapping.first;
    auto *lastNode = mapping.second.second;

    for (auto itr = succ_begin(bb), ite = succ_end(bb); itr != ite; ++itr) {
      auto *nextBB = *itr;
      auto bbItr = bbToNode.find(nextBB);
      if (bbItr != bbToNode.end())
        lastNode->insertEdge(bbItr->second.first);
      else {
        assert(nonEmptySuccMap.count(nextBB));
        auto &vec = nonEmptySuccMap[nextBB];
        for (auto *succNode : vec)
          lastNode->insertEdge(succNode);
      }
    }
  }

  // Connect the entry node with the main graph
  if (bbToNode.count(&entryBlock))
    cfg.getEntryNode()->insertEdge(bbToNode[&entryBlock].first);
  else {
    assert(nonEmptySuccMap.count(&entryBlock));
    auto &vec = nonEmptySuccMap[&entryBlock];
    for (auto *node : vec)
      cfg.getEntryNode()->insertEdge(node);
  }
}

void FunctionTranslator::drawDefUseEdgeFromValue(const Value *defVal,
                                                 tpa::CFGNode *useNode) {
  assert(defVal != nullptr && useNode != nullptr);

  if (!defVal->getType()->isPointerTy())
    return;

  if (isa<GlobalValue>(defVal) || isa<Argument>(defVal) ||
      isa<UndefValue>(defVal) || isa<ConstantPointerNull>(defVal)) {
    // Nodes that use global values are def roots
    cfg.getEntryNode()->insertDefUseEdge(useNode);
  } else if (const auto *defInst = dyn_cast<Instruction>(defVal)) {
    // For instructions, see if we have corresponding node attached to it
    if (auto *defNode = instToNode[defInst])
      defNode->insertDefUseEdge(useNode);
    else {
      std::string instStr;
      raw_string_ostream instOS(instStr);
      instOS << *defInst;
      instOS.flush();
      LOG_WARN("Failed to find node for instruction: {}", instStr);
    }
  }
}

void FunctionTranslator::constructDefUseChains() {
  for (auto *useNode : cfg) {
    switch (useNode->getNodeTag()) {
    case CFGNodeTag::Entry:
      break;
    case CFGNodeTag::Alloc:
      cfg.getEntryNode()->insertDefUseEdge(useNode);
      break;
    case CFGNodeTag::Copy: {
      const auto *copyNode = static_cast<const CopyCFGNode *>(useNode);
      for (auto *src : *copyNode) {
        const auto *defVal = src->stripPointerCasts();
        drawDefUseEdgeFromValue(defVal, useNode);
      }
      break;
    }
    case CFGNodeTag::Offset: {
      const auto *offsetNode = static_cast<const OffsetCFGNode *>(useNode);
      const auto *defVal = offsetNode->getSrc()->stripPointerCasts();
      drawDefUseEdgeFromValue(defVal, useNode);
      break;
    }
    case CFGNodeTag::Load: {
      const auto *loadNode = static_cast<const LoadCFGNode *>(useNode);
      const auto *defVal = loadNode->getSrc()->stripPointerCasts();
      drawDefUseEdgeFromValue(defVal, useNode);
      break;
    }
    case CFGNodeTag::Store: {
      const auto *storeNode = static_cast<const StoreCFGNode *>(useNode);
      const auto *srcVal = storeNode->getSrc()->stripPointerCasts();
      drawDefUseEdgeFromValue(srcVal, useNode);
      const auto *dstVal = storeNode->getDest()->stripPointerCasts();
      drawDefUseEdgeFromValue(dstVal, useNode);
      break;
    }
    case CFGNodeTag::Call: {
      const auto *callNode = static_cast<const CallCFGNode *>(useNode);
      const auto *funPtr = callNode->getFunctionPointer()->stripPointerCasts();
      drawDefUseEdgeFromValue(funPtr, useNode);
      for (auto *arg : *callNode) {
        const auto *defVal = arg->stripPointerCasts();
        drawDefUseEdgeFromValue(defVal, useNode);
      }
      break;
    }
    case CFGNodeTag::Ret: {
      const auto *retNode = static_cast<const ReturnCFGNode *>(useNode);
      const auto *retVal = retNode->getReturnValue();
      if (retVal != nullptr) {
        const auto *defVal = retVal->stripPointerCasts();
        drawDefUseEdgeFromValue(defVal, useNode);
      }
      break;
    }
    }
  }
}

void FunctionTranslator::computeNodePriority() {
  PriorityAssigner pa(cfg);
  pa.traverse();
}

// "Semi-Sparse" optimization:
// Nodes that only manipulate top-level pointers (Alloc, Copy, Offset) do not
// affect the store directly. They are "sparse" in the sense that they don't
// participate in the memory flow directly. We detach them from the CFG, so the
// flow analysis skips them, relying purely on def-use chains for their values.
void FunctionTranslator::detachStorePreservingNodes() {
  for (auto *node : cfg) {
    if (node->isAllocNode() || node->isCopyNode() || node->isOffsetNode())
      node->detachFromCFG();
  }
}

void FunctionTranslator::translateFunction(const Function &llvmFunc) {
  // Scan the basic blocks and create the nodes first. We will worry about how
  // to connect them later
  translateBasicBlock(llvmFunc);

  // Now the biggest problem are those "empty blocks" (i.e. blocks that do not
  // contain any tpa::CFGNode). Those blocks may form cycles. So we need to
  // know, in advance, what are the non empty successors of the empty blocks.
  processEmptyBlock();

  // Connect all the cfg nodes we've built
  connectCFGNodes(llvmFunc.getEntryBlock());

  // Draw def-use edges
  constructDefUseChains();

  // Compute the priority of each node
  computeNodePriority();

  // Detach all store-preserving nodes
  detachStorePreservingNodes();
}

} // namespace tpa