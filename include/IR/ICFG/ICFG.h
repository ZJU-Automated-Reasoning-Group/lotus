/// @file ICFG.h
/// @brief Interprocedural Control-Flow Graph (ICFG) representation.
///
/// This file defines the ICFG class which extends LLVM's basic CFG to support
/// interprocedural analysis by connecting call sites to callee entry/exit
/// points.

#pragma once

#include <llvm/Analysis/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <unordered_set>

// #include <iostream>

#include "IR/ICFG/ICFGEdge.h"
#include "IR/ICFG/ICFGNode.h"
#include "Utils/LLVM/GenericGraph.h"

/// @brief Interprocedural Control-Flow Graph (ICFG).
///
/// Extends basic CFG with interprocedural edges (call/return) to enable
/// whole-program control flow analysis.
typedef GenericGraph<ICFGNode, ICFGEdge> GenericICFGTy;
class ICFG : public GenericICFGTy {

public:
  using ICFGNodeIDToNodeMapTy =
      std::unordered_map<NodeID, ICFGNode *>;
  using iterator = ICFGNodeIDToNodeMapTy::iterator;
  using const_iterator = ICFGNodeIDToNodeMapTy::const_iterator;

  using blockToIntraNodeMapTy =
      std::unordered_map<const llvm::BasicBlock *, IntraBlockNode *>;
  using functionToEntryIntraNodeMapTy =
      std::unordered_map<const llvm::Function *, IntraBlockNode *>;
  using functionToEntryNodeMapTy =
      std::unordered_map<const llvm::Function *, FunEntryBlockNode *>;
  using functionToExitNodeMapTy =
      std::unordered_map<const llvm::Function *, FunExitBlockNode *>;
  using functionToUnwindExitNodeMapTy =
      std::unordered_map<const llvm::Function *, FunUnwindExitBlockNode *>;
  using callToRetNodeMapTy =
      std::unordered_map<const llvm::Instruction *, CallRetBlockNode *>;
  using callToUnwindNodeMapTy =
      std::unordered_map<const llvm::Instruction *, CallUnwindBlockNode *>;

  NodeID totalICFGNode;

private:
  blockToIntraNodeMapTy blockToIntraNodeMap;
  functionToEntryIntraNodeMapTy functionToEntryIntraNodeMap;
  functionToEntryNodeMapTy functionToEntryNodeMap;
  functionToExitNodeMapTy functionToExitNodeMap;
  functionToUnwindExitNodeMapTy functionToUnwindExitNodeMap;
  callToRetNodeMapTy callToRetNodeMap;
  callToUnwindNodeMapTy callToUnwindNodeMap;
  GlobalInitBlockNode *globalInitNode = nullptr;

public:
  /// @brief Constructs an empty ICFG.
  ICFG();

  /// @brief Destructor.
  virtual ~ICFG() {}

  /// @brief Retrieves an ICFG node by its ID.
  /// @param id Node identifier.
  /// @return Pointer to the ICFG node.
  inline ICFGNode *getICFGNode(NodeID id) const { return getGNode(id); }

  /// @brief Checks if an ICFG node with the given ID exists.
  /// @param id Node identifier.
  /// @return True if the node exists.
  inline bool hasICFGNode(NodeID id) const { return hasGNode(id); }

  /// @brief Checks if an intraprocedural edge exists between two nodes.
  /// @param src Source node.
  /// @param dst Destination node.
  /// @param kind Edge kind.
  /// @return Pointer to the edge if it exists, nullptr otherwise.
  ICFGEdge *hasIntraICFGEdge(ICFGNode *src, ICFGNode *dst,
                             ICFGEdge::ICFGEdgeK kind);

  /// @brief Checks if an interprocedural edge exists between two nodes.
  /// @param src Source node.
  /// @param dst Destination node.
  /// @param kind Edge kind.
  /// @param cs Optional callsite for call/return edges.
  /// @return Pointer to the edge if it exists, nullptr otherwise.
  ICFGEdge *hasInterICFGEdge(ICFGNode *src, ICFGNode *dst,
                             ICFGEdge::ICFGEdgeK kind,
                             const llvm::Instruction *cs = nullptr);

  /// @brief Retrieves an edge between two nodes.
  /// @param src Source node.
  /// @param dst Destination node.
  /// @param kind Edge kind.
  /// @param cs Optional callsite for call/return edges.
  /// @return Pointer to the edge, or nullptr if not found.
  ICFGEdge *getICFGEdge(const ICFGNode *src, const ICFGNode *dst,
                        ICFGEdge::ICFGEdgeK kind,
                        const llvm::Instruction *cs = nullptr);

  /// @brief Gets the mapping from functions to their entry nodes.
  /// @return Const reference to the map of function to entry node.
  inline const functionToEntryIntraNodeMapTy &getFunctionEntryMap() const {
    return functionToEntryIntraNodeMap;
  }

public:
  /// @brief Removes an ICFG edge from the graph.
  /// @param edge Edge to remove.
  inline void removeICFGEdge(ICFGEdge *edge) {
    edge->getDstNode()->removeIncomingEdge(edge);
    edge->getSrcNode()->removeOutgoingEdge(edge);
    delete edge;
  }

  /// @brief Removes an ICFG node and all its incident edges from the graph.
  /// @param node Node to remove.
  inline void removeICFGNode(ICFGNode *node) {
    if (!node)
      return;

    // Collect edges first to avoid iterator invalidation during removal.
    std::vector<ICFGEdge *> edgesToRemove;
    std::unordered_set<ICFGEdge *> seenEdges;
    for (auto *e : node->getOutEdges())
      if (seenEdges.insert(e).second)
        edgesToRemove.push_back(e);
    for (auto *e : node->getInEdges())
      if (seenEdges.insert(e).second)
        edgesToRemove.push_back(e);
    for (auto *e : edgesToRemove)
      removeICFGEdge(e);

    if (auto *intra = llvm::dyn_cast<IntraBlockNode>(node)) {
      blockToIntraNodeMap.erase(intra->getBasicBlock());
      const llvm::Function *F = intra->getFunction();
      auto it = functionToEntryIntraNodeMap.find(F);
      if (it != functionToEntryIntraNodeMap.end() && it->second == intra)
        functionToEntryIntraNodeMap.erase(it);
    } else if (auto *entry = llvm::dyn_cast<FunEntryBlockNode>(node)) {
      functionToEntryNodeMap.erase(entry->getFunction());
    } else if (llvm::isa<GlobalInitBlockNode>(node)) {
      globalInitNode = nullptr;
    } else if (auto *exit = llvm::dyn_cast<FunExitBlockNode>(node)) {
      functionToExitNodeMap.erase(exit->getFunction());
    } else if (auto *unwindExit =
                   llvm::dyn_cast<FunUnwindExitBlockNode>(node)) {
      functionToUnwindExitNodeMap.erase(unwindExit->getFunction());
    } else if (auto *ret = llvm::dyn_cast<CallRetBlockNode>(node)) {
      callToRetNodeMap.erase(ret->getCallSite());
    } else if (auto *unwind = llvm::dyn_cast<CallUnwindBlockNode>(node)) {
      callToUnwindNodeMap.erase(unwind->getCallSite());
    }

    removeGNode(node);
    delete node;
  }

  /// @brief Adds an intraprocedural edge between two nodes.
  /// @param srcNode Source node.
  /// @param dstNode Destination node.
  /// @return Pointer to the created edge, or nullptr if already exists.
  ICFGEdge *addIntraEdge(ICFGNode *srcNode, ICFGNode *dstNode);

  /// @brief Adds a call edge from caller to callee entry.
  /// @param srcNode Caller node.
  /// @param dstNode Callee entry node.
  /// @param cs Call instruction.
  /// @return Pointer to the created edge, or nullptr if already exists.
  ICFGEdge *addCallEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                        const llvm::Instruction *cs);

  /// @brief Adds a return edge from callee exit to caller.
  /// @param srcNode Callee exit node.
  /// @param dstNode Caller node.
  /// @param cs Call instruction.
  /// @return Pointer to the created edge, or nullptr if already exists.
  ICFGEdge *addRetEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                       const llvm::Instruction *cs);

  /// @brief Adds an exceptional return edge from callee unwind exit to caller.
  ICFGEdge *addExcRetEdge(ICFGNode *srcNode, ICFGNode *dstNode,
                          const llvm::Instruction *cs);

  /// @brief Verifies that both nodes of an intra edge belong to the same
  /// function.
  /// @param srcNode Source node.
  /// @param dstNode Destination node.
  inline void checkIntraEdgeParents(const ICFGNode *srcNode,
                                    const ICFGNode *dstNode) {
    auto *srcfun = srcNode->getFunction();
    auto *dstfun = dstNode->getFunction();
    if (srcfun != nullptr && dstfun != nullptr) {
      assert((srcfun == dstfun) &&
             "src and dst nodes of an intra edge should in the same function!");
    }
  }

  /// @brief Adds an ICFG edge to the graph.
  ///
  /// Returns true if the edge was newly inserted into both endpoint sets.
  /// Returns false (and deletes the edge) if it was already present.
  /// Asserts if the edge was only partially inserted, which indicates an
  /// inconsistency in the graph's in/out edge sets.
  /// @param edge Edge to add.
  /// @return True if successfully added, false if already existed.
  inline bool addICFGEdge(ICFGEdge *edge) {
    bool added1 = edge->getDstNode()->addIncomingEdge(edge);
    bool added2 = edge->getSrcNode()->addOutgoingEdge(edge);
    if (!added1 && !added2) {
      // Edge already exists in both sets — it's a true duplicate.
      delete edge;
      return false;
    }
    assert(added1 && added2 &&
           "edge partially inserted: graph in/out edge sets are inconsistent!");
    return true;
  }

  /// @brief Adds an ICFG node to the graph.
  /// @param node Node to add.
  virtual inline void addICFGNode(ICFGNode *node) {
    addGNode(node->getId(), node);
  }

  /// @brief Checks if an intra-block node exists for a basic block.
  /// @param bb Basic block.
  /// @return True if the node exists.
  bool hasIntraBlockNode(const llvm::BasicBlock *bb);

  /// @brief Gets or creates an intra-block node for a basic block.
  /// @param bb Basic block.
  /// @return Pointer to the ICFG node.
  IntraBlockNode *getIntraBlockNode(const llvm::BasicBlock *bb);

  /// @brief Gets or creates the dedicated function-entry node.
  FunEntryBlockNode *getFunEntryICFGNode(const llvm::Function *F);

  /// @brief Gets or creates the dedicated module-global initialization node.
  GlobalInitBlockNode *getGlobalInitICFGNode();

  /// @brief Gets or creates the dedicated function-exit node.
  FunExitBlockNode *getFunExitICFGNode(const llvm::Function *F);

  /// @brief Gets or creates the dedicated function unwind-exit node.
  FunUnwindExitBlockNode *getFunUnwindExitICFGNode(const llvm::Function *F);

  /// @brief Gets or creates the dedicated call return-site node.
  CallRetBlockNode *getRetICFGNode(const llvm::Instruction *callInst);

  /// @brief Gets or creates the dedicated call unwind-site node.
  CallUnwindBlockNode *getUnwindICFGNode(const llvm::Instruction *callInst);

private:
  /// Get/Add IntraBlock ICFGNode
  inline IntraBlockNode *getIntraBlockICFGNode(const llvm::BasicBlock *bb) {
    blockToIntraNodeMapTy::const_iterator it = blockToIntraNodeMap.find(bb);
    if (it == blockToIntraNodeMap.end())
      return nullptr;
    return it->second;
  }
  inline IntraBlockNode *addIntraBlockICFGNode(const llvm::BasicBlock *bb) {
    IntraBlockNode *sNode = new IntraBlockNode(totalICFGNode++, bb);
    addICFGNode(sNode);
    blockToIntraNodeMap[bb] = sNode;

    if (bb == &bb->getParent()->front()) {

      functionToEntryIntraNodeMap[bb->getParent()] = sNode;
    }

    return sNode;
  }

  inline FunEntryBlockNode *getFunEntryNode(const llvm::Function *F) {
    auto it = functionToEntryNodeMap.find(F);
    return it == functionToEntryNodeMap.end() ? nullptr : it->second;
  }

  inline GlobalInitBlockNode *getGlobalInitNode() { return globalInitNode; }

  inline FunExitBlockNode *getFunExitNode(const llvm::Function *F) {
    auto it = functionToExitNodeMap.find(F);
    return it == functionToExitNodeMap.end() ? nullptr : it->second;
  }

  inline FunUnwindExitBlockNode *getFunUnwindExitNode(
      const llvm::Function *F) {
    auto it = functionToUnwindExitNodeMap.find(F);
    return it == functionToUnwindExitNodeMap.end() ? nullptr : it->second;
  }

  inline CallRetBlockNode *getRetNode(const llvm::Instruction *callInst) {
    auto it = callToRetNodeMap.find(callInst);
    return it == callToRetNodeMap.end() ? nullptr : it->second;
  }

  inline CallUnwindBlockNode *getUnwindNode(
      const llvm::Instruction *callInst) {
    auto it = callToUnwindNodeMap.find(callInst);
    return it == callToUnwindNodeMap.end() ? nullptr : it->second;
  }

  FunEntryBlockNode *addFunEntryICFGNode(const llvm::Function *F);
  GlobalInitBlockNode *addGlobalInitICFGNode();
  FunExitBlockNode *addFunExitICFGNode(const llvm::Function *F);
  FunUnwindExitBlockNode *addFunUnwindExitICFGNode(const llvm::Function *F);
  CallRetBlockNode *addRetICFGNode(const llvm::Instruction *callInst);
  CallUnwindBlockNode *addUnwindICFGNode(const llvm::Instruction *callInst);
};
