/// @file PDGEdge.h
/// @brief Edge representation for Program Dependency Graph (PDG)
///
/// This file defines the Edge class that represents dependencies between nodes
/// in the PDG. Edges are typed (data, control, parameter dependencies) and
/// maintain bidirectional relationships between source and destination nodes.

#pragma once
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"

namespace pdg {
class Node;

/// @brief Edge in the Program Dependency Graph
///
/// Represents a dependency relationship between two nodes. Edges are typed to
/// distinguish between different kinds of dependencies (data, control,
/// parameter, etc.). Edges are stored in sets on nodes and must be comparable
/// for ordering.
class Edge {
private:
  EdgeType _edge_type;
  Node *_source;
  Node *_dst;

public:
  /// @brief Deleted default constructor (edges must have source, destination,
  /// and type)
  Edge() = delete;

  /// @brief Constructs an edge with specified source, destination, and type
  /// @param source The source node of the edge
  /// @param dst The destination node of the edge
  /// @param edge_type The type of dependency this edge represents
  Edge(Node *source, Node *dst, EdgeType edge_type) {
    _source = source;
    _dst = dst;
    _edge_type = edge_type;
  }

  /// @brief Copy constructor
  /// @param e The edge to copy
  Edge(const Edge &e) // copy constructor
  {
    _source = e.getSrcNode();
    _dst = e.getDstNode();
    _edge_type = e.getEdgeType();
  }

  /// @brief Gets the edge type
  /// @return The type of this edge (data, control, parameter, etc.)
  EdgeType getEdgeType() const { return _edge_type; }

  /// @brief Gets the source node
  /// @return Pointer to the source node of this edge
  Node *getSrcNode() const { return _source; }

  /// @brief Gets the destination node
  /// @return Pointer to the destination node of this edge
  Node *getDstNode() const { return _dst; }

  /// @brief Less-than comparison operator for ordering in sets
  ///
  /// Edges are compared based on their source node, destination node, and type.
  /// Two edges are considered equal for set purposes if they have the same
  /// source, destination, and type.
  ///
  /// @param e Edge to compare with
  /// @return True if this edge is considered less than the other edge
  bool operator<(const Edge &e) const {
    if (_source != e.getSrcNode())
      return _source < e.getSrcNode();
    if (_dst != e.getDstNode())
      return _dst < e.getDstNode();
    return static_cast<int>(_edge_type) < static_cast<int>(e.getEdgeType());
  }
};

} // namespace pdg
