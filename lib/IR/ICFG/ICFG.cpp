/// @file ICFG.cpp
/// @brief Implementation of ICFG node and edge operations.

#include "IR/ICFG/ICFG.h"

#include <iostream>

#include <llvm/IR/Instructions.h>

using namespace llvm;

namespace {

bool isExceptionalFunctionExitInst(const Instruction &inst) {
  if (isa<ResumeInst>(inst))
    return true;

  if (const auto *cleanupRet = dyn_cast<CleanupReturnInst>(&inst))
    return cleanupRet->unwindsToCaller();

  return false;
}

} // namespace

//
//=============================================================================
// ICFG Node
//=============================================================================
//

std::string ICFGNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "ICFGNode ID: " << getId();
  return rawstr.str();
}

void ICFGNode::dump() const {

  std::cout << this->toString() << "\n";
  std::cout << "OutEdges:\n";
  for (auto *edge : getOutEdges()) {

    std::cout << "\t" << edge->toString() << "\n";
  }
  std::cout << "InEdges:\n";
  for (auto *edge : getInEdges()) {

    std::cout << "\t" << edge->toString() << "\n";
  }
}

std::string IntraBlockNode::toString() const {

  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "IntraBlockNode ID: " << getId();
  if (getBasicBlock()->hasName()) {

    rawstr << ", Name: " << getBasicBlock()->getName().str();
  }

  return rawstr.str();
}

std::string GlobalInitBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "GlobalInitBlockNode ID: " << getId();
  return rawstr.str();
}

std::string FunEntryBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "FunEntryBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  return rawstr.str();
}

std::string FunExitBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "FunExitBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  return rawstr.str();
}

std::string FunUnwindExitBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "FunUnwindExitBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  return rawstr.str();
}

std::string CallRetBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "CallRetBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  if (callSite)
    rawstr << ", CallSite: " << *callSite;
  return rawstr.str();
}

std::string CallUnwindBlockNode::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "CallUnwindBlockNode ID: " << getId();
  if (getFunction())
    rawstr << ", Function: " << getFunction()->getName();
  if (callSite)
    rawstr << ", CallSite: " << *callSite;
  return rawstr.str();
}

//
//=============================================================================
// ICFG Edge
//=============================================================================
//

std::string ICFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "ICFGEdge: [" << getDstID() << "<--" << getSrcID() << "]\t";
  return rawstr.str();
}

std::string IntraCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "IntraCFGEdge: [" << getDstID() << "<--" << getSrcID() << "]\t";

  return rawstr.str();
}

std::string CallCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "CallCFGEdge " << " [";
  rawstr << getDstID() << "<--" << getSrcID() << "]\t CallSite: " << *cs
         << "\t";
  return rawstr.str();
}

std::string RetCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "RetCFGEdge " << " [";
  rawstr << getDstID() << "<--" << getSrcID() << "]\t CallSite: " << *cs
         << "\t";
  return rawstr.str();
}

std::string ExcRetCFGEdge::toString() const {
  std::string str;
  raw_string_ostream rawstr(str);
  rawstr << "ExcRetCFGEdge "
         << "[" << getDstID() << "<--" << getSrcID() << "]\t CallSite: "
         << *cs << "\t";
  return rawstr.str();
}

//
//=============================================================================
// ICFG
//=============================================================================
//

/// @brief Constructs an empty ICFG.
ICFG::ICFG() : totalICFGNode(0) {}

/// @brief Checks if an intraprocedural edge exists between two nodes.
ICFGEdge *ICFG::hasIntraICFGEdge(ICFGNode *src, ICFGNode *dst,
                                 ICFGEdge::ICFGEdgeK kind) {
  ICFGEdge edge(src, dst, kind);
  ICFGEdge *outEdge = src->hasOutgoingEdge(&edge);
  ICFGEdge *inEdge = dst->hasIncomingEdge(&edge);
  if (outEdge && inEdge) {
    assert(outEdge == inEdge && "edges not match");
    return outEdge;
  }
  return nullptr;
}

/// @brief Checks if an interprocedural edge exists between two nodes.
ICFGEdge *ICFG::hasInterICFGEdge(ICFGNode *src, ICFGNode *dst,
                                 ICFGEdge::ICFGEdgeK kind,
                                 const llvm::Instruction *cs) {
  return getICFGEdge(src, dst, kind, cs);
}

/// @brief Retrieves an edge between two nodes of a specific kind.
ICFGEdge *ICFG::getICFGEdge(const ICFGNode *src, const ICFGNode *dst,
                            ICFGEdge::ICFGEdgeK kind,
                            const llvm::Instruction *cs) {
  ICFGEdge *edge = nullptr;
  size_t counter = 0;
  for (auto iter = src->OutEdgeBegin(); iter != src->OutEdgeEnd(); ++iter) {
    if ((*iter)->getDstID() != dst->getId() || (*iter)->getEdgeKind() != kind)
      continue;
    if (cs && (*iter)->getCallSite() != cs)
      continue;
    if (!cs && (kind == ICFGEdge::CallCF || kind == ICFGEdge::RetCF ||
                kind == ICFGEdge::ExcRetCF) &&
        edge != nullptr && edge->getCallSite() != (*iter)->getCallSite()) {
      return nullptr;
    }
    if ((*iter)->getDstID() == dst->getId() && (*iter)->getEdgeKind() == kind) {
      counter++;
      edge = (*iter);
    }
  }
  assert((kind != ICFGEdge::IntraCF || counter <= 1) &&
         "there's more than one edge between two ICFG nodes");
  return edge;
}

/// @brief Adds an intraprocedural edge between two nodes.
ICFGEdge *ICFG::addIntraEdge(ICFGNode *srcNode, ICFGNode *dstNode) {
  checkIntraEdgeParents(srcNode, dstNode);
  if (hasIntraICFGEdge(srcNode, dstNode, ICFGEdge::IntraCF))
    return nullptr;
  IntraCFGEdge *intraEdge = new IntraCFGEdge(srcNode, dstNode);
  return addICFGEdge(intraEdge) ? intraEdge : nullptr;
}

/// @brief Adds an interprocedural call edge from caller to callee.
ICFGEdge *ICFG::addCallEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                            const llvm::Instruction *cs) {
  if (hasInterICFGEdge(srcNode, dstNode, ICFGEdge::CallCF, cs))
    return nullptr;
  CallCFGEdge *callEdge = new CallCFGEdge(srcNode, dstNode, cs);
  return addICFGEdge(callEdge) ? callEdge : nullptr;
}

/// @brief Adds an interprocedural return edge from callee to caller.
ICFGEdge *ICFG::addRetEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                           const llvm::Instruction *cs) {
  if (hasInterICFGEdge(srcNode, dstNode, ICFGEdge::RetCF, cs))
    return nullptr;
  RetCFGEdge *retEdge = new RetCFGEdge(srcNode, dstNode, cs);
  return addICFGEdge(retEdge) ? retEdge : nullptr;
}

ICFGEdge *ICFG::addExcRetEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                              const llvm::Instruction *cs) {
  if (hasInterICFGEdge(srcNode, dstNode, ICFGEdge::ExcRetCF, cs))
    return nullptr;
  auto *retEdge = new ExcRetCFGEdge(srcNode, dstNode, cs);
  return addICFGEdge(retEdge) ? retEdge : nullptr;
}

bool ICFG::hasIntraBlockNode(const llvm::BasicBlock *bb) {

  IntraBlockNode *node = getIntraBlockICFGNode(bb);
  return node != nullptr;
}

IntraBlockNode *ICFG::getIntraBlockNode(const llvm::BasicBlock *bb) {

  IntraBlockNode *node = getIntraBlockICFGNode(bb);
  if (node == nullptr)
    node = addIntraBlockICFGNode(bb);
  return node;
}

FunEntryBlockNode *ICFG::addFunEntryICFGNode(const llvm::Function *F) {
  if (!F || F->isDeclaration())
    return nullptr;
  const BasicBlock *bb = &F->getEntryBlock();
  auto *node = new FunEntryBlockNode(totalICFGNode++, bb);
  addICFGNode(node);
  functionToEntryNodeMap[F] = node;
  return node;
}

GlobalInitBlockNode *ICFG::addGlobalInitICFGNode() {
  if (globalInitNode)
    return globalInitNode;
  auto *node = new GlobalInitBlockNode(totalICFGNode++);
  addICFGNode(node);
  globalInitNode = node;
  return node;
}

FunExitBlockNode *ICFG::addFunExitICFGNode(const llvm::Function *F) {
  if (!F || F->isDeclaration())
    return nullptr;
  const BasicBlock *anchor = &F->getEntryBlock();
  for (const BasicBlock &bb : *F) {
    if (isa<ReturnInst>(bb.getTerminator())) {
      anchor = &bb;
      break;
    }
  }
  auto *node = new FunExitBlockNode(totalICFGNode++, anchor);
  addICFGNode(node);
  functionToExitNodeMap[F] = node;
  return node;
}

FunUnwindExitBlockNode *ICFG::addFunUnwindExitICFGNode(const llvm::Function *F) {
  if (!F || F->isDeclaration())
    return nullptr;

  const BasicBlock *anchor = &F->getEntryBlock();
  for (const BasicBlock &bb : *F) {
    const Instruction *terminator = bb.getTerminator();
    if (terminator && isExceptionalFunctionExitInst(*terminator)) {
      anchor = &bb;
      break;
    }
  }

  auto *node = new FunUnwindExitBlockNode(totalICFGNode++, anchor);
  addICFGNode(node);
  functionToUnwindExitNodeMap[F] = node;
  return node;
}

CallRetBlockNode *ICFG::addRetICFGNode(const llvm::Instruction *callInst) {
  if (!callInst)
    return nullptr;
  const BasicBlock *retBB = callInst->getParent();
  if (const auto *invokeInst = dyn_cast<InvokeInst>(callInst))
    retBB = invokeInst->getNormalDest();
  auto *node = new CallRetBlockNode(totalICFGNode++, callInst, retBB);
  addICFGNode(node);
  callToRetNodeMap[callInst] = node;
  return node;
}

CallUnwindBlockNode *ICFG::addUnwindICFGNode(
    const llvm::Instruction *callInst) {
  if (!callInst)
    return nullptr;
  const BasicBlock *unwindBB = callInst->getParent();
  if (const auto *invokeInst = dyn_cast<InvokeInst>(callInst))
    unwindBB = invokeInst->getUnwindDest();
  auto *node = new CallUnwindBlockNode(totalICFGNode++, callInst, unwindBB);
  addICFGNode(node);
  callToUnwindNodeMap[callInst] = node;
  return node;
}

FunEntryBlockNode *ICFG::getFunEntryICFGNode(const llvm::Function *F) {
  if (auto *node = getFunEntryNode(F))
    return node;
  return addFunEntryICFGNode(F);
}

GlobalInitBlockNode *ICFG::getGlobalInitICFGNode() {
  if (auto *node = getGlobalInitNode())
    return node;
  return addGlobalInitICFGNode();
}

FunExitBlockNode *ICFG::getFunExitICFGNode(const llvm::Function *F) {
  if (auto *node = getFunExitNode(F))
    return node;
  return addFunExitICFGNode(F);
}

FunUnwindExitBlockNode *ICFG::getFunUnwindExitICFGNode(
    const llvm::Function *F) {
  if (auto *node = getFunUnwindExitNode(F))
    return node;
  return addFunUnwindExitICFGNode(F);
}

CallRetBlockNode *ICFG::getRetICFGNode(const llvm::Instruction *callInst) {
  if (auto *node = getRetNode(callInst))
    return node;
  return addRetICFGNode(callInst);
}

CallUnwindBlockNode *ICFG::getUnwindICFGNode(
    const llvm::Instruction *callInst) {
  if (auto *node = getUnwindNode(callInst))
    return node;
  return addUnwindICFGNode(callInst);
}
