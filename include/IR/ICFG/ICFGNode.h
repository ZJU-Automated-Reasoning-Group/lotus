/// @file ICFGNode.h
/// @brief ICFG node representations for basic blocks.

#pragma once

#include "IR/ICFG/ICFGEdge.h"
#include "Utils/LLVM/GenericGraph.h"

#include <iostream>

#include <llvm/Analysis/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

class ICFGNode;

/// @brief Base class for interprocedural control-flow graph nodes.
///
/// Each node represents a program point in the ICFG (typically a basic block).
using GenericICFGNodeTy = GenericNode<ICFGNode, ICFGEdge>;

class ICFGNode : public GenericICFGNodeTy {

public:
  /// kinds of ICFG node
  enum ICFGNodeK {
    IntraBlock,
    GlobalInitBlock,
    FunEntryBlock,
    FunExitBlock,
    FunUnwindExitBlock,
    CallRetBlock,
    CallUnwindBlock
  };

public:
  /// @brief Constructs an ICFG node.
  /// @param i Node ID.
  /// @param k Node kind.
  ICFGNode(NodeID i, ICFGNodeK k)
      : GenericICFGNodeTy(i, k), _function(nullptr), _basic_block(nullptr) {}

  /// @brief Returns the function containing this node.
  /// @return Pointer to the parent function.
  virtual const llvm::Function *getFunction() const { return _function; }

  /// @brief Returns the basic block represented by this node.
  /// @return Pointer to the basic block.
  virtual const llvm::BasicBlock *getBasicBlock() const { return _basic_block; }

  /// @brief Stream operator for printing node information.
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &o,
                                       const ICFGNode &node) {
    o << node.toString();
    return o;
  }

  /// @brief Returns a string representation of this node.
  /// @return String description.
  virtual std::string toString() const;

  /// @brief Dumps node information to standard output.
  void dump() const;

protected:
  const llvm::Function *_function;
  const llvm::BasicBlock *_basic_block;
};

/// @brief ICFG node representing a basic block within a function.
///
/// This is the primary node type for intraprocedural control flow.
class IntraBlockNode : public ICFGNode {

public:
  /// @brief Constructs an intra-block node.
  /// @param id Node ID.
  /// @param bb Basic block this node represents.
  IntraBlockNode(NodeID id, const llvm::BasicBlock *bb)
      : ICFGNode(id, IntraBlock) {
    _basic_block = bb;
    _function = bb->getParent();
  }

  /// @brief Type inquiry support for LLVM-style RTTI.
  static inline bool classof(const IntraBlockNode *) { return true; }

  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == IntraBlock;
  }

  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == IntraBlock;
  }

  /// @brief Returns a string representation of this intra-block node.
  /// @return String description including block name.
  std::string toString() const;
};

/// @brief Dedicated module-global initialization node.
///
/// This synthetic node anchors whole-program global state before any root
/// function is entered. It has no owning function or basic block.
class GlobalInitBlockNode : public ICFGNode {
public:
  explicit GlobalInitBlockNode(NodeID id) : ICFGNode(id, GlobalInitBlock) {}

  static inline bool classof(const GlobalInitBlockNode *) { return true; }
  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == GlobalInitBlock;
  }
  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == GlobalInitBlock;
  }

  std::string toString() const;
};

/// @brief Dedicated function-entry node used for interprocedural summaries.
class FunEntryBlockNode : public ICFGNode {
public:
  FunEntryBlockNode(NodeID id, const llvm::BasicBlock *bb)
      : ICFGNode(id, FunEntryBlock) {
    _basic_block = bb;
    _function = bb ? bb->getParent() : nullptr;
  }

  static inline bool classof(const FunEntryBlockNode *) { return true; }
  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == FunEntryBlock;
  }
  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == FunEntryBlock;
  }

  std::string toString() const;
};

/// @brief Dedicated function-exit node shared by all returns of a function.
class FunExitBlockNode : public ICFGNode {
public:
  FunExitBlockNode(NodeID id, const llvm::BasicBlock *bb)
      : ICFGNode(id, FunExitBlock) {
    _basic_block = bb;
    _function = bb ? bb->getParent() : nullptr;
  }

  static inline bool classof(const FunExitBlockNode *) { return true; }
  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == FunExitBlock;
  }
  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == FunExitBlock;
  }

  std::string toString() const;
};

/// @brief Dedicated function unwind-exit node shared by all escaping EH exits.
class FunUnwindExitBlockNode : public ICFGNode {
public:
  FunUnwindExitBlockNode(NodeID id, const llvm::BasicBlock *bb)
      : ICFGNode(id, FunUnwindExitBlock) {
    _basic_block = bb;
    _function = bb ? bb->getParent() : nullptr;
  }

  static inline bool classof(const FunUnwindExitBlockNode *) { return true; }
  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == FunUnwindExitBlock;
  }
  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == FunUnwindExitBlock;
  }

  std::string toString() const;
};

/// @brief Dedicated call return-site node keyed by call instruction.
class CallRetBlockNode : public ICFGNode {
  const llvm::Instruction *callSite;

public:
  CallRetBlockNode(NodeID id, const llvm::Instruction *cs,
                   const llvm::BasicBlock *bb)
      : ICFGNode(id, CallRetBlock), callSite(cs) {
    _basic_block = bb;
    _function = cs ? cs->getFunction() : (bb ? bb->getParent() : nullptr);
  }

  const llvm::Instruction *getCallSite() const { return callSite; }

  static inline bool classof(const CallRetBlockNode *) { return true; }
  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == CallRetBlock;
  }
  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == CallRetBlock;
  }

  std::string toString() const;
};

/// @brief Dedicated call unwind-site node keyed by call instruction.
class CallUnwindBlockNode : public ICFGNode {
  const llvm::Instruction *callSite;

public:
  CallUnwindBlockNode(NodeID id, const llvm::Instruction *cs,
                      const llvm::BasicBlock *bb)
      : ICFGNode(id, CallUnwindBlock), callSite(cs) {
    _basic_block = bb;
    _function = cs ? cs->getFunction() : (bb ? bb->getParent() : nullptr);
  }

  const llvm::Instruction *getCallSite() const { return callSite; }

  static inline bool classof(const CallUnwindBlockNode *) { return true; }
  static inline bool classof(const ICFGNode *node) {
    return node->getNodeKind() == CallUnwindBlock;
  }
  static inline bool classof(const GenericICFGNodeTy *node) {
    return node->getNodeKind() == CallUnwindBlock;
  }

  std::string toString() const;
};
