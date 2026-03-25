//===- SVFGBuilderEdges.cpp -- SVFG Edge Building Implementation
//---------------------------//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//
//
// This file contains intra-procedural edge building methods for SVFGBuilder
//
//===----------------------------------------------------------------------===//

#include "IR/SVFG/SVFGBuilder.h"

using namespace lotus::analysis;
using namespace llvm;

void SVFGBuilder::buildEdges() {
  buildDirectEdges();
  buildCopyEdges();
  buildGepEdges();
  buildPhiEdges();
  buildCmpEdges();
  buildBranchEdges();
  buildMemoryEdges(); // This calls buildLoadEdges() and buildStoreEdges()
}

void SVFGBuilder::buildDirectEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      if (!isa<BinaryOpSVFGNode>(dstNode) && !isa<UnaryOpSVFGNode>(dstNode))
        continue;

      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        auto srcIt = valueToNode.find(opVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (!srcNode || srcNode == dstNode)
          continue;

        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildCopyEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        const Value *opVal = op.get();
        auto srcIt = valueToNode.find(opVal);
        if (srcIt == valueToNode.end())
          continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (!srcNode)
          continue;

        if (isa<LoadInst>(&inst) || isa<StoreInst>(&inst) ||
            isa<GetElementPtrInst>(&inst) || isa<PHINode>(&inst) ||
            isa<SelectInst>(&inst) || isa<CallBase>(&inst) ||
            isa<CmpInst>(&inst) || isa<BranchInst>(&inst) ||
            isa<BinaryOperator>(&inst)) {
          continue;
        }

        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraCopy);
      }
    }
  }
}

void SVFGBuilder::buildLoadEdges() {
  for (const auto &pair : loadToLoadNode) {
    const LoadInst *load = pair.first;
    auto dstIt = loadToLoadNode.find(load);
    SVFGNode *dstNode = svfg->getNode(dstIt->second);
    if (!dstNode)
      continue;

    const Value *ptr = load->getPointerOperand();
    auto ptrIt = valueToNode.find(ptr);
    if (ptrIt != valueToNode.end()) {
      if (SVFGNode *ptrNode = svfg->getNode(ptrIt->second)) {
        svfg->addEdge(ptrNode, dstNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildStoreEdges() {
  for (const auto &pair : storeToStoreNode) {
    const StoreInst *store = pair.first;
    auto srcIt = storeToStoreNode.find(store);
    SVFGNode *srcNode = svfg->getNode(srcIt->second);
    if (!srcNode)
      continue;

    const Value *ptr = store->getPointerOperand();
    auto ptrIt = valueToNode.find(ptr);
    if (ptrIt != valueToNode.end()) {
      if (SVFGNode *ptrNode = svfg->getNode(ptrIt->second)) {
        svfg->addEdge(ptrNode, srcNode, SVFGEdgeK::IntraDirect);
      }
    }
    const Value *val = store->getValueOperand();
    auto valIt = valueToNode.find(val);
    if (valIt != valueToNode.end()) {
      if (SVFGNode *valNode = svfg->getNode(valIt->second)) {
        svfg->addEdge(valNode, srcNode, SVFGEdgeK::IntraDirect);
      }
    }
  }
}

void SVFGBuilder::buildGepEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      if (!isa<GetElementPtrInst>(&inst))
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;

      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      if (inst.getNumOperands() == 0)
        continue;
      const Value *ptr = inst.getOperand(0);
      auto srcIt = valueToNode.find(ptr);
      if (srcIt == valueToNode.end())
        continue;

      SVFGNode *srcNode = svfg->getNode(srcIt->second);
      if (srcNode) {
        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraGep);
      }
    }
  }
}

void SVFGBuilder::buildPhiEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;

    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

      for (const Instruction &inst : *bb) {
        if (!isa<PHINode>(&inst) && !isa<SelectInst>(&inst))
          continue;

        auto dstIt = valueToNode.find(&inst);
        if (dstIt == valueToNode.end())
          continue;

        SVFGNode *dstNode = svfg->getNode(dstIt->second);
        if (!dstNode || !dstNode->isPhiNode())
          continue;

        for (const Use &op : inst.operands()) {
          const Value *incomingVal = op.get();
          auto srcIt = valueToNode.find(incomingVal);
          if (srcIt == valueToNode.end())
            continue;

        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (srcNode) {
          svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraPhi);
        }
      }
    }
  }
}

void SVFGBuilder::buildCmpEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;
    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      if (!isa<CmpInst>(&inst))
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;
      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      for (const Use &op : inst.operands()) {
        auto srcIt = valueToNode.find(op.get());
        if (srcIt == valueToNode.end())
          continue;
        SVFGNode *srcNode = svfg->getNode(srcIt->second);
        if (srcNode)
          svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraCmp);
      }
    }
  }
}

void SVFGBuilder::buildBranchEdges() {
  for (auto &pair : *icfg) {
    ICFGNode *node = pair.second;
    IntraBlockNode *blockNode = dyn_cast<IntraBlockNode>(node);
    if (!blockNode)
      continue;
    const BasicBlock *bb = blockNode->getBasicBlock();
    if (!bb)
      continue;

    for (const Instruction &inst : *bb) {
      const auto *br = dyn_cast<BranchInst>(&inst);
      if (!br || !br->isConditional())
        continue;

      auto dstIt = valueToNode.find(&inst);
      if (dstIt == valueToNode.end())
        continue;
      SVFGNode *dstNode = svfg->getNode(dstIt->second);
      if (!dstNode)
        continue;

      const Value *cond = br->getCondition();
      auto srcIt = valueToNode.find(cond);
      if (srcIt == valueToNode.end())
        continue;
      SVFGNode *srcNode = svfg->getNode(srcIt->second);
      if (srcNode)
        svfg->addEdge(srcNode, dstNode, SVFGEdgeK::IntraBranch);
    }
  }
}

void SVFGBuilder::buildMemoryEdges() {
  buildLoadEdges();
  buildStoreEdges();
}
