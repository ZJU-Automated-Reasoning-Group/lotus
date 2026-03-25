/**
 * @file AliasGraph.h
 * @brief Alias graph data structure for must-alias analysis (CC'18)
 *
 * Implements the optimized data structure from:
 *   "An Efficient Data Structure for Must-Alias Analysis"
 *   Kastrinis et al., CC'18
 *
 * The alias graph represents must-alias equivalence classes and field
 * relationships compactly: nodes are abstract objects (alias classes)
 * holding sets of variables; edges are field-labeled (node --f--> node).
 * Two variables must-alias iff they are in the same node. Access paths
 * var.fld1.fld2... are represented implicitly as paths in the graph.
 *
 * Key operations:
 * - Move(x, y): merge nodes of x and y
 * - Store(x.f = z): add edge from x's node (field f) to z's node
 * - Load(z = y.g): new node for z, edge from y's node (field g) to it
 * - intersect(g1, g2): merge-point intersection (alias holds iff in both)
 * - gc(g): remove nodes that encode no useful alias pairs
 * - allAliases(ap): find all access paths that must-alias ap
 */

#ifndef UNDERAPPROX_ALIASGRAPH_H
#define UNDERAPPROX_ALIASGRAPH_H

#include <cstdint>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>

namespace UnderApprox {

/// Variable identifier (e.g., SSA value index or pointer).
using VarId = unsigned;
/// Field label (e.g., struct field index or GEP path hash).
using FieldLabel = uint32_t;

/// Node identifier in the alias graph.
using NodeId = unsigned;

/// Sentinel for "no node".
constexpr NodeId kNoNode = static_cast<NodeId>(-1);

/**
 * Alias graph: nodes = alias classes (sets of variables), edges = field links.
 * Invariants:
 * - Two variables are in the same node iff they must-alias.
 * - Path from node N following edges f1,f2,... represents access path
 * base.f1.f2...
 * - Two paths reaching the same node => those access paths must-alias.
 */
class AliasGraph {
public:
  AliasGraph() = default;

  /// Copy constructor (deep copy).
  AliasGraph(const AliasGraph &Other);
  AliasGraph &operator=(const AliasGraph &Other);

  /// Move constructor/assignment.
  AliasGraph(AliasGraph &&) noexcept = default;
  AliasGraph &operator=(AliasGraph &&) noexcept = default;

  // --- Node and variable management -----------------------------------------

  /// Add a new node containing exactly the given variable; returns its NodeId.
  /// If the variable is already in some node, that node is returned (no
  /// duplicate).
  NodeId addVariable(VarId V);

  /// Get the node that contains variable V, or kNoNode if not present.
  NodeId getNode(VarId V) const;

  /// Merge the nodes containing variables X and Y (Move semantics).
  /// After this, X and Y are in the same node. Returns the unified node id.
  NodeId moveMerge(VarId X, VarId Y);

  /// Add edge: from node of base variable (field F) to node of target variable.
  /// If base's node already has an edge for F, it is overwritten (strong-update
  /// style for this abstract operation). Used for Store(base.f = target).
  void storeEdge(VarId Base, FieldLabel F, VarId Target);

  /// Load semantics: variable Z now points to the object at base.field F.
  /// Z is removed from its current node and represented by a fresh node.
  /// If base has no F-edge, create base --F--> Z; otherwise merge Z with the
  /// existing F-edge target so Z aliases base.F. Used for Load(Z = base.F).
  void loadEdge(VarId Base, FieldLabel F, VarId Z);

  /// Rename variable OldId to NewId (paper §3: "rename variables in alias
  /// classes"). OldId is removed from its node; NewId is placed in that node.
  /// If NewId already exists in another node, the two nodes are merged first.
  void renameVariable(VarId OldId, VarId NewId);

  /// Number of nodes (including empty ones until gc).
  size_t numNodes() const { return nodes_.size(); }

  /// Number of variables that have a node (max VarId ever used is not stored).
  size_t numVariables() const { return varToNode_.size(); }

  /// Get the set of variables in a node (empty if node is empty or invalid).
  llvm::SmallVector<VarId, 4> getNodeVars(NodeId N) const;

  /// Check if node N is empty (no variables).
  bool nodeEmpty(NodeId N) const;

  /// Forward edge: from node N, following field F, which node? Returns kNoNode
  /// if none.
  NodeId getTarget(NodeId N, FieldLabel F) const;

  /// Predecessors of node N: pairs (source node, field label).
  void getPredecessors(
      NodeId N, llvm::SmallVector<std::pair<NodeId, FieldLabel>, 4> &Out) const;

  // --- High-level algorithms from the paper ---------------------------------

  /// Intersect two alias graphs (for control-flow merge).
  /// Alias holds in result iff it holds in both g1 and g2.
  /// Result nodes are (i,j) with vars = vars(i) ∩ vars(j); edges when both
  /// graphs have matching edges. Empty nodes with no in-edges are eagerly
  /// removed (paper §4.1).
  static AliasGraph intersect(const AliasGraph &G1, const AliasGraph &G2);

  /// Garbage-collect nodes that do not encode useful alias pairs (paper §4.1).
  /// Removes: single-variable nodes with no in/out edges; empty nodes with
  /// zero in-edges or one in-edge and zero out-edges.
  void gc();

  /// Find all access paths (up to maxLength fields) that must-alias the given
  /// access path (base variable + sequence of fields). Appends (VarId, path) to
  /// Out.
  void allAliases(
      VarId Base, const llvm::SmallVectorImpl<FieldLabel> &Path,
      unsigned maxLength,
      llvm::SmallVector<std::pair<VarId, llvm::SmallVector<FieldLabel, 4>>, 8>
          &Out) const;

  /// Check whether two access paths must-alias: base1 + path1 and base2 +
  /// path2.
  bool mustAliasAccessPath(
      VarId Base1, const llvm::SmallVectorImpl<FieldLabel> &Path1, VarId Base2,
      const llvm::SmallVectorImpl<FieldLabel> &Path2) const;

private:
  struct Node {
    llvm::SmallSet<VarId, 4> Vars;
    llvm::DenseMap<FieldLabel, NodeId> OutEdges;
  };
  std::vector<Node> nodes_;
  /// Variable -> node that contains it (at most one node per variable).
  llvm::DenseMap<VarId, NodeId> varToNode_;
  /// Reverse edges: (target node) -> [(source node, field)].
  std::vector<llvm::SmallVector<std::pair<NodeId, FieldLabel>, 2>> inEdges_;

  NodeId addNode();
  void addEdge(NodeId From, FieldLabel F, NodeId To);
  void addInEdge(NodeId To, NodeId From, FieldLabel F);
  void removeInEdge(NodeId To, NodeId From, FieldLabel F);
  NodeId mergeNodes(NodeId I, NodeId J);
  void removeVarFromNode(NodeId N, VarId V);
  void ensureInEdgesCapacity(NodeId N);
  void rebuildWithoutNodes(const llvm::SmallSet<NodeId, 16> &Remove);
  /// Remove only empty nodes with no in-edges (paper §4.1 eager elimination).
  void gcEmptyNodesWithNoInEdges();
};

} // end namespace UnderApprox

#endif
