/**
 * @file NodeFactory.h
 * @brief Node and node-factory types for Andersen's constraint graph.
 *
 * ## Design
 *
 * Every pointer-relevant value in the program is represented by one or more
 * **`AndersNode`** objects.  There are two kinds of nodes:
 *
 *  - **VALUE_NODE**: represents a pointer-typed SSA value (e.g., the result
 *    of an `alloca`, a function argument, or a `load` of a pointer).
 *  - **OBJ_NODE**: represents a memory object (an allocation site — global,
 *    stack, or heap).
 *
 * Nodes are identified by a dense integer index (`NodeIndex`).  The index
 * space is managed by `AndersNodeFactory`, which is the **only** way to
 * create nodes.  This ensures that indices are contiguous and consistent
 * across the analysis.
 *
 * ## Context Sensitivity
 *
 * `AndersNodeFactory` supports context-sensitive analysis via a `CtxKey`
 * (an opaque `const void *`).  Each (value, context) pair gets its own
 * node, allowing the analysis to distinguish the same pointer in different
 * calling contexts.  Context-insensitive analysis uses a single global
 * context key.
 *
 * ## Special Nodes
 *
 * Four special nodes are pre-allocated at fixed indices:
 *  - **UniversalPtr** (index 0): a pointer that may point to anything.
 *    Used as a conservative over-approximation for unknown pointers.
 *  - **UniversalObj** (index 1): the "universal" memory object.
 *    UniversalPtr points to UniversalObj.
 *  - **NullPtr** (index 2): the null pointer constant.
 *  - **NullObject** (index 3): the object pointed to by null (used to
 *    model null dereferences without crashing the analysis).
 *
 * ## Node Merging
 *
 * During constraint solving, nodes that are found to be equivalent (same
 * points-to set) are merged via `mergeNode(n0, n1)`.  After merging,
 * `getMergeTarget(n)` returns the representative of `n`'s equivalence class.
 * Merged nodes are not removed from the `nodes` vector; they simply have
 * their `mergeTarget` field updated.
 */

#ifndef ANDERSEN_NODE_FACTORY_H
#define ANDERSEN_NODE_FACTORY_H

#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Value.h>

/// Dense integer index type for constraint-graph nodes.
using NodeIndex = unsigned;

/**
 * @class AndersNode
 * @brief A single node in Andersen's constraint graph.
 *
 * Each node represents either a pointer-typed SSA value (VALUE_NODE) or a
 * memory object / allocation site (OBJ_NODE).  Due to offline optimisations
 * (pointer/location equivalence), some nodes are "artificial" — they have no
 * corresponding LLVM `Value` and exist solely to represent a shared
 * points-to set for a group of equivalent nodes.
 *
 * @note Clients must not construct `AndersNode` directly.  All nodes must be
 *       created through `AndersNodeFactory` to guarantee index consistency.
 */
class AndersNode {
public:
  /// Discriminator between pointer-value nodes and memory-object nodes.
  enum AndersNodeType {
    VALUE_NODE, ///< Represents a pointer-typed SSA value.
    OBJ_NODE    ///< Represents a memory object (allocation site).
  };

private:
  AndersNodeType type;   ///< Whether this is a value node or an object node.
  NodeIndex idx;         ///< This node's unique index in the factory's vector.
  NodeIndex mergeTarget; ///< Index of the representative after merging (== idx
                         ///< if not merged).
  const llvm::Value *value; ///< The LLVM value this node corresponds to, or
                            ///< nullptr for artificial nodes.

  AndersNode(AndersNodeType t, unsigned i, const llvm::Value *v = nullptr)
      : type(t), idx(i), mergeTarget(i), value(v) {}

public:
  /// @brief Return this node's index in the factory.
  NodeIndex getIndex() const { return idx; }
  /// @brief Return the LLVM value associated with this node, or nullptr.
  const llvm::Value *getValue() const { return value; }

  friend class AndersNodeFactory;
};

/**
 * @class AndersNodeFactory
 * @brief Factory and registry for all `AndersNode` objects.
 *
 * Maintains a flat vector of all nodes and several maps for looking up nodes
 * by (value, context) pair.  The factory is the single source of truth for
 * node indices; all other analysis components refer to nodes by index.
 *
 * Context sensitivity is implemented by keying all maps on a `CtxKey`
 * (opaque pointer).  Context-insensitive analysis uses a single shared key.
 */
class AndersNodeFactory {
public:
  /// Sentinel value returned by lookup methods when a node is not found.
  static const unsigned InvalidIndex;

  /// Opaque context token.  For context-insensitive analysis, all nodes
  /// share a single global context key.
  using CtxKey = const void *;

private:
  /// All nodes, indexed by `NodeIndex`.  The vector is append-only.
  std::vector<AndersNode> nodes;

  // Fixed indices for the four pre-allocated special nodes.
  static const NodeIndex UniversalPtrIndex =
      0; ///< Index of the universal pointer node.
  static const NodeIndex UniversalObjIndex =
      1; ///< Index of the universal object node.
  static const NodeIndex NullPtrIndex = 2; ///< Index of the null pointer node.
  static const NodeIndex NullObjectIndex =
      3; ///< Index of the null object node.

  /// Per-context map from LLVM value → value-node index.
  using ValueNodeMap = llvm::DenseMap<const llvm::Value *, NodeIndex>;
  llvm::DenseMap<CtxKey, ValueNodeMap> valueNodeMap;
  /// Per-context map from LLVM value → object-node index.
  llvm::DenseMap<CtxKey, ValueNodeMap> objNodeMap;
  /// Per-context map from function → return-value node index.
  llvm::DenseMap<CtxKey, llvm::DenseMap<const llvm::Function *, NodeIndex>>
      returnMap;
  /// Per-context map from function → vararg node index.
  llvm::DenseMap<CtxKey, llvm::DenseMap<const llvm::Function *, NodeIndex>>
      varargMap;

  /// Reverse map: LLVM value → all node indices across all contexts.
  /// Used by `getValueNodesFor` to support context-insensitive union queries.
  llvm::DenseMap<const llvm::Value *, std::vector<NodeIndex>> valueNodeBuckets;

public:
  AndersNodeFactory();

  // Factory methods (context-aware)
  NodeIndex createValueNode(const llvm::Value *val, CtxKey ctx);
  NodeIndex createObjectNode(const llvm::Value *val, CtxKey ctx);
  NodeIndex createReturnNode(const llvm::Function *f, CtxKey ctx);
  NodeIndex createVarargNode(const llvm::Function *f, CtxKey ctx);

  // Map lookup interfaces (return InvalidIndex if value not found)
  NodeIndex getValueNodeFor(const llvm::Value *val, CtxKey ctx) const;
  NodeIndex getValueNodeForConstant(const llvm::Constant *c, CtxKey ctx) const;
  NodeIndex getObjectNodeFor(const llvm::Value *val, CtxKey ctx) const;
  NodeIndex getObjectNodeForConstant(const llvm::Constant *c, CtxKey ctx) const;
  NodeIndex getReturnNodeFor(const llvm::Function *f, CtxKey ctx) const;
  NodeIndex getVarargNodeFor(const llvm::Function *f, CtxKey ctx) const;

  // Query all value nodes across contexts
  void getValueNodesFor(const llvm::Value *val,
                        std::vector<NodeIndex> &out) const;

  // Node merge interfaces
  void mergeNode(NodeIndex n0, NodeIndex n1); // Merge n1 into n0
  NodeIndex getMergeTarget(NodeIndex n);
  NodeIndex getMergeTarget(NodeIndex n) const;

  // Pointer arithmetic
  bool isObjectNode(NodeIndex i) const {
    return (nodes.at(i).type == AndersNode::OBJ_NODE);
  }
  NodeIndex getOffsetObjectNode(NodeIndex n, unsigned offset) const {
    assert(isObjectNode(n + offset));
    return n + offset;
  }

  // Special node getters
  NodeIndex getUniversalPtrNode() const { return UniversalPtrIndex; }
  NodeIndex getUniversalObjNode() const { return UniversalObjIndex; }
  NodeIndex getNullPtrNode() const { return NullPtrIndex; }
  NodeIndex getNullObjectNode() const { return NullObjectIndex; }

  // Value getters
  const llvm::Value *getValueForNode(NodeIndex i) const {
    return nodes.at(i).getValue();
  }
  void getAllocSites(std::vector<const llvm::Value *> &) const;

  // Value remover
  void removeNodeForValue(const llvm::Value *val) {
    valueNodeBuckets.erase(val);
  }

  // Size getters
  unsigned getNumNodes() const { return nodes.size(); }

  // For debugging purpose
  void dumpNode(NodeIndex) const;
  void dumpNodeInfo() const;
  void dumpRepInfo() const;
};

#endif
