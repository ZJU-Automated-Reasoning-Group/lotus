/**
 * @file SparseBitVectorGraph.h
 * @brief Sparse-bit-vector constraint graph for Andersen's analysis.
 *
 * ## Design
 *
 * `SparseBitVectorGraph` is the **online constraint graph** used during the
 * constraint-solving phase of Andersen's analysis.  Each node in the graph
 * corresponds to a `NodeIndex` and its outgoing edges (successors) are
 * stored as a `llvm::SparseBitVector`.
 *
 * Using a sparse bit-vector for edges gives O(1) edge insertion and O(N/W)
 * edge union (where W is the word size), which is critical for the
 * worklist-based propagation loop.
 *
 * ## Iterator Stability
 *
 * The node map uses `std::unordered_map` (not `llvm::DenseMap`) to guarantee
 * **iterator stability**: inserting a new node does not invalidate iterators
 * or pointers to existing nodes.  This is required because the solver may
 * call `getOrInsertNode()` while iterating over the graph.
 *
 * ## AndersGraphTraits Specialisation
 *
 * A full `AndersGraphTraits<SparseBitVectorGraph>` specialisation is provided
 * at the bottom of this file, enabling `CycleDetector` to operate on this
 * graph type without any additional boilerplate.
 */

#ifndef ANDERSEN_SPARSEBITVECTOR_GRAPH_H
#define ANDERSEN_SPARSEBITVECTOR_GRAPH_H

#include "Alias/SparrowAA/GraphTraits.h"
#include "Alias/SparrowAA/NodeFactory.h"

#include <algorithm>
#include <unordered_map>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SparseBitVector.h>

/**
 * @class SparseBitVectorGraphNode
 * @brief A single node in the sparse-bit-vector constraint graph.
 *
 * Stores the node's index and its set of successor node indices as a
 * `llvm::SparseBitVector`.  Successor edges represent inclusion constraints:
 * an edge from A to B means "A's points-to set must include B's points-to set".
 *
 * @note Only `SparseBitVectorGraph` may construct nodes.
 */
class SparseBitVectorGraphNode {
private:
  NodeIndex idx; ///< This node's index in the constraint graph.
  llvm::SparseBitVector<>
      succs; ///< Set of successor node indices (outgoing edges).

  /// Insert a directed edge to @p n.  Called only by `SparseBitVectorGraph`.
  void insertEdge(NodeIndex n) { return succs.set(n); }

  SparseBitVectorGraphNode(NodeIndex i) : idx(i) {}

public:
  using iterator = llvm::SparseBitVector<>::iterator;

  /// @brief Return this node's index.
  NodeIndex getNodeIndex() const { return idx; }

  /// @brief Iterator over successor node indices.
  iterator begin() const { return succs.begin(); }
  iterator end() const { return succs.end(); }

  /// @brief Return the number of outgoing edges (not O(1)).
  unsigned succ_getSize() const { return succs.count(); }

  friend class SparseBitVectorGraph;
};

/**
 * @class SparseBitVectorGraph
 * @brief Directed graph with sparse-bit-vector successor sets.
 *
 * Used as the online constraint graph during Andersen's solving phase.
 * Nodes are created on demand via `getOrInsertNode()`.  The `mergeEdge()`
 * operation efficiently unions the successor sets of two nodes, which is
 * the core operation when collapsing SCCs.
 */
class SparseBitVectorGraph {
private:
  /// `std::unordered_map` is used (not `llvm::DenseMap`) to guarantee
  /// iterator stability: inserting a new node must not invalidate pointers
  /// to existing nodes, since the solver may insert while iterating.
  using NodeMapTy = std::unordered_map<NodeIndex, SparseBitVectorGraphNode>;
  NodeMapTy graph;

public:
  using iterator = NodeMapTy::iterator;
  using const_iterator = NodeMapTy::const_iterator;

private:
  iterator getOrInsertNodeMap(NodeIndex idx) {
    auto itr = graph.find(idx);
    if (itr == graph.end())
      itr = graph.insert(std::make_pair(idx, SparseBitVectorGraphNode(idx)))
                .first;
    return itr;
  }

public:
  SparseBitVectorGraph() {}

  SparseBitVectorGraphNode *getOrInsertNode(NodeIndex idx) {
    auto itr = getOrInsertNodeMap(idx);
    return &(itr->second);
  }

  void insertEdge(NodeIndex src, NodeIndex dst) {
    auto itr = getOrInsertNodeMap(src);
    (itr->second).insertEdge(dst);
  }

  // src's successors += dst's successors
  void mergeEdge(NodeIndex src, NodeIndex dst) {
    auto dstItr = graph.find(dst);
    if (dstItr == graph.end())
      return;

    auto srcItr = getOrInsertNodeMap(src);
    (srcItr->second).succs |= (dstItr->second).succs;
  }

  SparseBitVectorGraphNode *getNodeWithIndex(NodeIndex idx) {
    auto itr = graph.find(idx);
    if (itr == graph.end())
      return nullptr;
    return &(itr->second);
  }

  unsigned getSize() const { return graph.size(); }

  void releaseMemory() { graph.clear(); }

  iterator begin() { return graph.begin(); }
  iterator end() { return graph.end(); }
  const_iterator begin() const { return graph.begin(); }
  const_iterator end() const { return graph.end(); }
};

// Specialize the AnderGraphTraits for SparseBitVectorGraph
template <> class AndersGraphTraits<SparseBitVectorGraph> {
public:
  using NodeType = SparseBitVectorGraphNode;
  using NodeIterator = MapValueIterator<SparseBitVectorGraph::iterator>;
  using ChildIterator = SparseBitVectorGraphNode::iterator;

  static inline ChildIterator child_begin(NodeType *n) { return n->begin(); }
  static inline ChildIterator child_end(NodeType *n) { return n->end(); }

  static inline NodeIterator node_begin(SparseBitVectorGraph *g) {
    return NodeIterator(g->begin());
  }
  static inline NodeIterator node_end(SparseBitVectorGraph *g) {
    return NodeIterator(g->end());
  }
};

#endif
