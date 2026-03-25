/**
 * @file PDGNode.cpp
 * @brief Implementation of the Node class for the Program Dependency Graph
 *
 * This file implements the Node class which represents vertices in the PDG.
 * Each node is typically associated with an LLVM Value (instruction, variable,
 * function, etc.) and maintains connections to other nodes through incoming and
 * outgoing edges.
 *
 * The Node class provides methods for:
 * - Adding neighbor nodes with specific edge types
 * - Querying incoming and outgoing edges and neighbors
 * - Filtering neighbors based on edge types
 * - Checking for specific neighbor relationships
 *
 * The edge types represent different kinds of dependencies (control, data,
 * parameter passing, etc.) between program elements.
 */

#include "IR/PDG/Core/PDGNode.h"

using namespace llvm;

/**
 * @brief Adds a directed edge to a neighbor node.
 *
 * Creates a new Edge object of the specified type connecting this node to the
 * neighbor. Updates both the outgoing edges of this node and the incoming edges
 * of the neighbor. Avoids duplicate edges of the same type to the same
 * neighbor.
 *
 * @param neighbor The destination node.
 * @param edge_type The type of dependency (e.g., CONTROLDEP_BR, DATA_DEF_USE).
 */
void pdg::Node::addNeighbor(Node &neighbor, EdgeType edge_type) {
  if (hasOutNeighborWithEdgeType(neighbor, edge_type))
    return;
  Edge *edge = new Edge(this, &neighbor, edge_type);
  addOutEdge(*edge);
  neighbor.addInEdge(*edge);
}

/**
 * @brief Retrieves the set of all nodes that have edges pointing to this node.
 *
 * @return A set of pointers to source nodes of incoming edges.
 */
std::set<pdg::Node *> pdg::Node::getInNeighbors() {
  std::set<Node *> in_neighbors;
  for (auto *edge : _in_edge_set) {
    in_neighbors.insert(edge->getSrcNode());
  }
  return in_neighbors;
}

std::set<pdg::Node *>
pdg::Node::getInNeighborsWithDepType(pdg::EdgeType edge_type) {
  std::set<Node *> in_neighbors_with_dep_type;
  for (auto *edge : _in_edge_set) {
    if (edge->getEdgeType() == edge_type)
      in_neighbors_with_dep_type.insert(edge->getSrcNode());
  }
  return in_neighbors_with_dep_type;
}

std::set<pdg::Node *> pdg::Node::getOutNeighbors() {
  std::set<Node *> out_neighbors;
  for (auto *edge : _out_edge_set) {
    out_neighbors.insert(edge->getDstNode());
  }
  return out_neighbors;
}

/**
 * @brief Retrieves outgoing neighbors connected by a specific edge type.
 *
 * @param edge_type The edge type to filter by.
 * @return A set of pointers to matching destination nodes.
 */
std::set<pdg::Node *>
pdg::Node::getOutNeighborsWithDepType(pdg::EdgeType edge_type) {
  std::set<Node *> out_neighbors_with_dep_type;
  for (auto *edge : _out_edge_set) {
    if (edge->getEdgeType() == edge_type)
      out_neighbors_with_dep_type.insert(edge->getDstNode());
  }
  return out_neighbors_with_dep_type;
}

bool pdg::Node::hasInNeighborWithEdgeType(Node &n, EdgeType edge_type) {
  for (auto *e : _in_edge_set) {
    if (e->getSrcNode() == &n && e->getEdgeType() == edge_type)
      return true;
  }
  return false;
}

bool pdg::Node::hasOutNeighborWithEdgeType(Node &n, EdgeType edge_type) {
  for (auto *e : _out_edge_set) {
    if (e->getDstNode() == &n && e->getEdgeType() == edge_type)
      return true;
  }
  return false;
}
