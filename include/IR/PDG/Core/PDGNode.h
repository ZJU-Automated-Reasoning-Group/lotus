/// @file PDGNode.h
/// @brief Core graph node and iterator classes for Program Dependency Graph
/// (PDG)
///
/// This file defines the fundamental Node class and EdgeIterator template that
/// form the backbone of the PDG data structure. Nodes represent program
/// elements (instructions, variables, functions) and maintain bidirectional
/// edge connections for dependency tracking.

#pragma once
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Support/LLVMEssentials.h"

#include <iterator>
#include <set>

namespace pdg {
/// @brief Forward declaration of edge iterator template
template <typename NodeTy> class EdgeIterator;

/// @brief Forward declaration of Edge class
class Edge;

/// @brief Core node class in the Program Dependency Graph
///
/// Nodes represent program elements (values, instructions, functions) in the
/// PDG. Each node maintains bidirectional edge connections for tracking data,
/// control, and parameter dependencies. Nodes can be associated with debug
/// information (DIType) to enable type-aware analysis.
class Node {
public:
  using EdgeSet = std::set<Edge *>;
  using iterator = EdgeIterator<Node>;
  using const_iterator = EdgeIterator<Node>;

  /// @brief Constructs a node without an associated LLVM value
  /// @param node_type Type of the node (formal in/out, actual in/out, global,
  /// etc.)
  Node(GraphNodeType node_type) {
    _val = nullptr;
    _node_type = node_type;
    _is_visited = false;
    _func = nullptr;
    _node_di_type = nullptr;
  }

  /// @brief Constructs a node associated with an LLVM value
  /// @param v The LLVM value this node represents
  /// @param node_type Type of the node (formal in/out, actual in/out, global,
  /// etc.)
  Node(llvm::Value &v, GraphNodeType node_type) {
    _val = &v;
    if (auto *inst = llvm::dyn_cast<llvm::Instruction>(&v))
      _func = inst->getFunction();
    else
      _func = nullptr;
    _node_type = node_type;
    _is_visited = false;
    _node_di_type = nullptr;
  }

  /// @brief Adds an incoming edge to this node
  /// @param e Edge to add (edge's destination must be this node)
  void addInEdge(Edge &e) { _in_edge_set.insert(&e); }

  /// @brief Adds an outgoing edge from this node
  /// @param e Edge to add (edge's source must be this node)
  void addOutEdge(Edge &e) { _out_edge_set.insert(&e); }

  /// @brief Gets the set of incoming edges
  /// @return Reference to the set of incoming edges
  EdgeSet &getInEdgeSet() { return _in_edge_set; }
  const EdgeSet &getInEdgeSet() const { return _in_edge_set; }

  /// @brief Gets the set of outgoing edges
  /// @return Reference to the set of outgoing edges
  EdgeSet &getOutEdgeSet() { return _out_edge_set; }
  const EdgeSet &getOutEdgeSet() const { return _out_edge_set; }

  /// @brief Sets the node type
  /// @param node_type Type of the node (formal in/out, actual in/out, global,
  /// etc.)
  void setNodeType(GraphNodeType node_type) { _node_type = node_type; }

  /// @brief Gets the node type
  /// @return The node type
  GraphNodeType getNodeType() const { return _node_type; }

  /// @brief Checks if this node has been visited (for graph traversal)
  /// @return True if visited, false otherwise
  bool isVisited() const { return _is_visited; }

  /// @brief Gets the function containing this node (if any)
  /// @return Pointer to the containing function, or nullptr
  llvm::Function *getFunc() const { return _func; }

  /// @brief Sets the function containing this node
  /// @param f The containing function
  void setFunc(llvm::Function &f) { _func = &f; }

  /// @brief Gets the LLVM value associated with this node
  /// @return Pointer to the LLVM value, or nullptr
  llvm::Value *getValue() { return _val; }
  const llvm::Value *getValue() const { return _val; }

  /// @brief Gets the debug information type for this node
  /// @return Pointer to the DIType, or nullptr
  llvm::DIType *getDIType() const { return _node_di_type; }

  /// @brief Sets the debug information type for this node
  /// @param di_type The DIType to associate with this node
  void setDIType(llvm::DIType &di_type) { _node_di_type = &di_type; }

  /// @brief Adds a neighbor with a specific edge type
  /// @param neighbor The neighbor node to connect to
  /// @param edge_type The type of edge to create
  void addNeighbor(Node &neighbor, EdgeType edge_type);

  /// @brief Iterator to the beginning of outgoing edges
  /// @return Iterator to first outgoing edge
  EdgeSet::iterator begin() { return _out_edge_set.begin(); }

  /// @brief Iterator to the end of outgoing edges
  /// @return Iterator past the last outgoing edge
  EdgeSet::iterator end() { return _out_edge_set.end(); }

  /// @brief Const iterator to the beginning of outgoing edges
  /// @return Const iterator to first outgoing edge
  EdgeSet::const_iterator begin() const { return _out_edge_set.begin(); }

  /// @brief Const iterator to the end of outgoing edges
  /// @return Const iterator past the last outgoing edge
  EdgeSet::const_iterator end() const { return _out_edge_set.end(); }

  /// @brief Gets all predecessor (incoming neighbor) nodes
  /// @return Set of predecessor nodes
  std::set<Node *> getInNeighbors();

  /// @brief Gets predecessor nodes with a specific edge type
  /// @param edge_type The edge type to filter by
  /// @return Set of predecessor nodes with the specified edge type
  std::set<Node *> getInNeighborsWithDepType(EdgeType edge_type);

  /// @brief Gets all successor (outgoing neighbor) nodes
  /// @return Set of successor nodes
  std::set<Node *> getOutNeighbors();

  /// @brief Gets successor nodes with a specific edge type
  /// @param edge_type The edge type to filter by
  /// @return Set of successor nodes with the specified edge type
  std::set<Node *> getOutNeighborsWithDepType(EdgeType edge_type);

  /// @brief Checks if there's an incoming edge from a specific node with a
  /// specific type
  /// @param n The potential predecessor node
  /// @param edge_type The edge type to check for
  /// @return True if such an edge exists
  bool hasInNeighborWithEdgeType(Node &n, EdgeType edge_type);

  /// @brief Checks if there's an outgoing edge to a specific node with a
  /// specific type
  /// @param n The potential successor node
  /// @param edge_type The edge type to check for
  /// @return True if such an edge exists
  bool hasOutNeighborWithEdgeType(Node &n, EdgeType edge_type);

  /// @brief Virtual destructor
  virtual ~Node() = default;

protected:
  llvm::Value *_val;
  llvm::Function *_func;
  bool _is_visited;
  EdgeSet _in_edge_set;
  EdgeSet _out_edge_set;
  GraphNodeType _node_type;
  llvm::DIType *_node_di_type;
};

/// @brief Iterator for traversing graph nodes via their outgoing edges
///
/// This iterator provides STL-like iteration over nodes reachable from a source
/// node by following outgoing edges. Used extensively in graph visualization
/// (dot printer) and graph traversal algorithms.
///
/// @tparam NodeTy The node type (typically Node or derived class)
template <typename NodeTy>
class EdgeIterator : public std::iterator<std::input_iterator_tag, NodeTy> {
  typename Node::EdgeSet::iterator _edge_iter;
  typedef EdgeIterator<NodeTy> this_type;

public:
  /// @brief Constructs an iterator pointing to the beginning of the edge set
  /// @param N The node whose edges to iterate over
  EdgeIterator(NodeTy *N) : _edge_iter(N->begin()) {}

  /// @brief Constructs an iterator pointing to the end of the edge set
  /// @param N The node whose edges to iterate over
  /// @param ignored Boolean flag to differentiate from begin constructor
  /// (unused)
  EdgeIterator(NodeTy *N, bool) : _edge_iter(N->end()) {}

  /// @brief Pre-increment operator
  /// @return Reference to this iterator
  this_type &operator++() {
    _edge_iter++;
    return *this;
  }

  /// @brief Post-increment operator
  /// @return Copy of the iterator before increment
  this_type operator++(int) {
    this_type old = *this;
    _edge_iter++;
    return old;
  }

  /// @brief Dereference operator
  /// @return Pointer to the destination node of the current edge
  Node *operator*() { return (*_edge_iter)->getDstNode(); }

  /// @brief Arrow operator
  /// @return Pointer to the destination node of the current edge
  Node *operator->() { return operator*(); }

  /// @brief Inequality comparison
  /// @param r Iterator to compare with
  /// @return True if iterators point to different positions
  bool operator!=(const this_type &r) const {
    return _edge_iter != r._edge_iter;
  }

  /// @brief Equality comparison
  /// @param r Iterator to compare with
  /// @return True if iterators point to the same position
  bool operator==(const this_type &r) const { return !(operator!=(r)); }

  /// @brief Gets the edge type of the current edge
  /// @return The type of the edge being iterated over
  EdgeType getEdgeType() { return (*_edge_iter)->getEdgeType(); }
};

} // namespace pdg
