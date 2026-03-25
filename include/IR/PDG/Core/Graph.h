/// @file Graph.h
/// @brief Core graph container classes for Program Dependency Graph (PDG)
///
/// This file defines GenericGraph (abstract base class) and ProgramGraph
/// (singleton) that serve as containers for nodes and edges in the PDG.
/// GenericGraph provides common graph operations while ProgramGraph adds
/// program-specific functionality for handling functions, calls, and debug
/// information.

#pragma once
#include "IR/PDG/Core/CallWrapper.h"
#include "IR/PDG/Core/FunctionWrapper.h"
#include "IR/PDG/Core/PDGEdge.h"
#include "IR/PDG/Core/PDGEnums.h"
#include "IR/PDG/Core/PDGNode.h"
#include "IR/PDG/Core/Tree.h"
#include "IR/PDG/Support/PDGCommandLineOptions.h"

#include <unordered_map>
#include <unordered_set>
// #include <map>
#include <set>

namespace pdg {
class Node;
class Edge;

/// @brief Abstract base class for graph containers
///
/// Provides common functionality for managing nodes and edges, including
/// lookup operations, reachability queries, and iteration support.
/// Derived classes implement the build() method to construct the graph
/// from an LLVM module.
class GenericGraph {
public:
  using ValueNodeMap = std::unordered_map<llvm::Value *, Node *>;
  using EdgeSet = std::set<Edge *>;
  using NodeSet = std::set<Node *>;

  /// @brief Gets iterator to beginning of value-node map
  /// @return Iterator to first entry
  ValueNodeMap::iterator val_node_map_begin() { return _val_node_map.begin(); }

  /// @brief Gets iterator to end of value-node map
  /// @return Iterator past last entry
  ValueNodeMap::iterator val_node_map_end() { return _val_node_map.end(); }

  /// @brief Default constructor
  GenericGraph() = default;

  /// @brief Gets iterator to beginning of node set
  /// @return Iterator to first node
  NodeSet::iterator begin() { return _node_set.begin(); }

  /// @brief Gets iterator to end of node set
  /// @return Iterator past last node
  NodeSet::iterator end() { return _node_set.end(); }

  /// @brief Gets const iterator to beginning of node set
  /// @return Const iterator to first node
  NodeSet::iterator begin() const { return _node_set.begin(); }

  /// @brief Gets const iterator to end of node set
  /// @return Const iterator past last node
  NodeSet::iterator end() const { return _node_set.end(); }

  /// @brief Builds the graph from an LLVM module
  /// @param M The LLVM module to process
  virtual void build(llvm::Module &M) = 0;

  /// @brief Adds an edge to the graph
  /// @param e Edge to add
  void addEdge(Edge &e) {
    _edge_set.insert(&e);
    bumpEpoch();
  }

  /// @brief Adds a node to the graph
  /// @param n Node to add
  void addNode(Node &n) {
    _node_set.insert(&n);
    bumpEpoch();
  }

  /// @brief Gets the node associated with an LLVM value
  /// @param v The LLVM value to look up
  /// @return Pointer to the node, or nullptr if not found
  Node *getNode(llvm::Value &v);

  /// @brief Checks if a node exists for an LLVM value
  /// @param v The LLVM value to check
  /// @return True if a node exists for this value
  bool hasNode(llvm::Value &v);

  /// @brief Gets the number of edges in the graph
  /// @return Edge count
  int numEdge() { return _edge_set.size(); }

  /// @brief Gets the number of nodes in the graph
  /// @return Node count
  int numNode() { return _val_node_map.size(); }

  /// @brief Marks the graph as built
  void setIsBuild() { _is_build = true; }

  /// @brief Checks if the graph has been built
  /// @return True if the graph is built
  bool isBuild() { return _is_build; }

  /// @brief Resets the graph to empty state
  ///
  /// Edges are heap-allocated in Node::addNeighbor() and stored only in the
  /// nodes' in/out edge sets; _edge_set is never populated and must not be
  /// used for deletion.  We collect edges from every node's out-edge set
  /// (each edge appears exactly once there) and delete them before deleting
  /// the nodes themselves.
  void reset() {
    bumpEpoch();
    _is_build = false;
    // Collect every edge exactly once via out-edge sets.
    std::unordered_set<Edge *> edges_to_delete;
    for (auto *node : _node_set) {
      if (!node)
        continue;
      for (auto *edge : node->getOutEdgeSet()) {
        if (edge)
          edges_to_delete.insert(edge);
      }
    }
    for (auto *edge : edges_to_delete) {
      delete edge;
    }
    for (auto *node : _node_set) {
      delete node;
    }
    _val_node_map.clear();
    _edge_set.clear();
    _node_set.clear();
  }

  unsigned long long getEpoch() const { return _epoch; }

  /// @brief Checks if there is a path from src to dst
  /// @param src Source node
  /// @param dst Destination node
  /// @return True if dst is reachable from src
  bool canReach(pdg::Node &src, pdg::Node &dst);

  /// @brief Checks if there is a path from src to dst, excluding certain edge
  /// types
  /// @param src Source node
  /// @param dst Destination node
  /// @param exclude_edge_types Edge types to exclude from the search
  /// @return True if dst is reachable from src using allowed edges
  bool canReach(pdg::Node &src, pdg::Node &dst,
                std::set<EdgeType> exclude_edge_types);

  /// @brief Gets the value-to-node mapping
  /// @return Reference to the unordered map
  ValueNodeMap &getValueNodeMap() { return _val_node_map; }

  /// @brief Dumps the graph (typically to stderr)
  void dumpGraph();

protected:
  void bumpEpoch() { ++_epoch; }

  ValueNodeMap _val_node_map;
  EdgeSet _edge_set;
  NodeSet _node_set;
  bool _is_build = false;
  unsigned long long _epoch = 1;
};

/// @brief Program-specific graph container (singleton)
///
/// Extends GenericGraph with program-specific features including:
/// - Function wrapper management for intra-procedural analysis
/// - Call wrapper management for inter-procedural analysis
/// - Debug information (DIType) binding for type-aware analysis
/// - Class hierarchy support for object-oriented programs
///
/// This is a singleton class - use getInstance() to access.
class ProgramGraph : public GenericGraph {
public:
  using FuncWrapperMap =
      std::unordered_map<llvm::Function *, FunctionWrapper *>;
  using CallWrapperMap = std::unordered_map<llvm::CallBase *, CallWrapper *>;
  using ClassNodeMap = std::unordered_map<std::string, Node *>;
  using NodeDIMap = std::unordered_map<Node *, llvm::DIType *>;

  /// @brief Default constructor
  ProgramGraph() = default;

  /// @brief Deleted copy constructor (singleton)
  ProgramGraph(const ProgramGraph &) = delete;

  /// @brief Deleted move constructor (singleton)
  ProgramGraph(ProgramGraph &&) = delete;

  /// @brief Deleted copy assignment (singleton)
  ProgramGraph &operator=(const ProgramGraph &) = delete;

  /// @brief Deleted move assignment (singleton)
  ProgramGraph &operator=(ProgramGraph &&) = delete;

  /// @brief Gets the singleton instance
  /// @return Reference to the singleton ProgramGraph
  static ProgramGraph &getInstance() {
    static ProgramGraph g{};
    return g;
  }

  /// @brief Gets the function wrapper map
  /// @return Reference to the map of functions to their wrappers
  FuncWrapperMap &getFuncWrapperMap() { return _func_wrapper_map; }

  /// @brief Gets the call wrapper map
  /// @return Reference to the map of call instructions to their wrappers
  CallWrapperMap &getCallWrapperMap() { return _call_wrapper_map; }

  /// @brief Gets the node-to-DIType map
  /// @return Reference to the map of nodes to their debug types
  NodeDIMap &getNodeDIMap() { return _node_di_type_map; }

  /// @brief Checks if the graph was built for a specific module
  /// @param M The module to check
  /// @return True if this graph was built from the given module
  bool isBuiltForModule(const llvm::Module &M) const {
    return _is_build && _built_module == &M;
  }

  /// @brief Builds the program graph from an LLVM module
  /// @param M The LLVM module to process
  void build(llvm::Module &M) override;

  /// @brief Checks if a function wrapper exists for a function
  /// @param F The function to check
  /// @return True if a wrapper exists
  bool hasFuncWrapper(llvm::Function &F) {
    return _func_wrapper_map.find(&F) != _func_wrapper_map.end();
  }

  /// @brief Checks if a call wrapper exists for a call instruction
  /// @param ci The call instruction to check
  /// @return True if a wrapper exists
  bool hasCallWrapper(llvm::CallBase &ci) {
    return _call_wrapper_map.find(&ci) != _call_wrapper_map.end();
  }

  /// @brief Gets the function wrapper for a function
  /// @param F The function
  /// @return Pointer to the function wrapper
  FunctionWrapper *getFuncWrapper(llvm::Function &F) {
    auto it = _func_wrapper_map.find(&F);
    if (it == _func_wrapper_map.end())
      return nullptr;
    return it->second;
  }

  /// @brief Gets the call wrapper for a call instruction
  /// @param ci The call instruction
  /// @return Pointer to the call wrapper
  CallWrapper *getCallWrapper(llvm::CallBase &ci) {
    auto it = _call_wrapper_map.find(&ci);
    if (it == _call_wrapper_map.end())
      return nullptr;
    return it->second;
  }

  /// @brief Binds debug information types to nodes in the graph
  /// @param M The LLVM module to process
  void bindDITypeToNodes(llvm::Module &M);

  /// @brief Computes the debug type for a node
  /// @param n The node to compute type for
  /// @return The computed DIType, or nullptr
  llvm::DIType *computeNodeDIType(Node &n);

  /// @brief Adds tree nodes to the graph (for field-sensitive analysis)
  /// @param tree The tree to add
  void addTreeNodesToGraph(Tree &tree);

  /// @brief Adds formal parameter tree nodes for a function
  /// @param func_w The function wrapper to process
  void addFormalTreeNodesToGraph(FunctionWrapper &func_w);

  /// @brief Checks if an instruction is an annotation call
  /// @param inst The instruction to check
  /// @return True if this is an annotation call
  bool isAnnotationCallInst(llvm::Instruction &inst);

  /// @brief Builds nodes for global annotations
  /// @param M The LLVM module to process
  void buildGlobalAnnotationNodes(llvm::Module &M);

  /// @brief Gets the class node map
  /// @return Reference to the map of class names to class nodes
  ClassNodeMap &getClassNodeMap() { return _class_node_map; }

  /// @brief Gets a class node by its name
  /// @param cls_name The class name
  /// @return Pointer to the class node, or nullptr if not found
  Node *getClassNodeByName(std::string cls_name);

  /// @brief Resets the graph and all its mappings
  ///
  /// Tree wrapper objects are owned by FunctionWrapper/CallWrapper, while the
  /// TreeNode objects they reference are also inserted into GenericGraph's
  /// _node_set and deleted there. Releasing the wrapper Trees first prevents
  /// leaking them across rebuilds without affecting node ownership.
  void reset() {
    // Step 1: release Tree wrapper objects from wrappers.
    for (auto &entry : _func_wrapper_map) {
      if (entry.second)
        entry.second->releaseTrees();
    }
    for (auto &entry : _call_wrapper_map) {
      if (entry.second)
        entry.second->releaseTrees();
    }
    // Step 2: delete wrapper objects (trees already released, so no
    // double-free).
    for (auto &entry : _call_wrapper_map) {
      delete entry.second;
    }
    for (auto &entry : _func_wrapper_map) {
      delete entry.second;
    }
    // Step 3: delete all nodes and edges.
    GenericGraph::reset();
    _func_wrapper_map.clear();
    _call_wrapper_map.clear();
    _node_di_type_map.clear();
    _class_node_map.clear();
    _class_name_set.clear();
    _built_module = nullptr;
  }

private:
  FuncWrapperMap _func_wrapper_map;
  CallWrapperMap _call_wrapper_map;
  NodeDIMap _node_di_type_map;
  ClassNodeMap _class_node_map;
  std::set<std::string> _class_name_set;
  llvm::Module *_built_module = nullptr;
};
} // namespace pdg
