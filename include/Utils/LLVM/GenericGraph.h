#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>

typedef int32_t NodeID;

/*!
 * Generic edge on the graph as base class
 */
template <class NodeTy> class GenericEdge {

public:
  /// Edge type
  typedef int32_t GEdgeKind;

private:
  NodeTy *src;        ///< source node
  NodeTy *dst;        ///< destination node
  GEdgeKind edgeKind; ///< edge kind

public:
  /// Constructor
  GenericEdge(NodeTy *s, NodeTy *d, GEdgeKind k)
      : src(s), dst(d), edgeKind(k) {}

  /// Destructor
  virtual ~GenericEdge() {}

  ///  get methods of the components
  //@{
  inline NodeID getSrcID() const { return src->getId(); }
  inline NodeID getDstID() const { return dst->getId(); }
  inline GEdgeKind getEdgeKind() const { return edgeKind; }
  NodeTy *getSrcNode() const { return src; }
  NodeTy *getDstNode() const { return dst; }
  //@}

  /// Add the hash function for std::set (we also can overload operator< to
  /// implement this)
  //  and duplicated elements in the set are not inserted (binary tree
  //  comparison)
  //@{
  typedef struct equalGEdge {
    bool operator()(const GenericEdge<NodeTy> *lhs,
                    const GenericEdge<NodeTy> *rhs) const {
      if (lhs->edgeKind != rhs->edgeKind)
        return lhs->edgeKind < rhs->edgeKind;
      else if (lhs->getSrcNode() != rhs->getSrcNode())
        return lhs->getSrcNode() < rhs->getSrcNode();
      else
        return lhs->getDstNode() < rhs->getDstNode();
    }
  } equalGEdge;

  inline bool operator==(const GenericEdge<NodeTy> *rhs) const {
    return (rhs->edgeKind == this->edgeKind &&
            rhs->getSrcNode() == this->getSrcNode() &&
            rhs->getDstNode() == this->getDstNode());
  }
  //@}
};

/*!
 * Generic node on the graph as base class
 */
template <class NodeTy, class EdgeTy> class GenericNode {

public:
  /// Edge kind
  typedef int32_t GNodeK;
  typedef std::set<EdgeTy *, typename EdgeTy::equalGEdge> GEdgeSetTy;
  /// Edge iterator
  ///@{
  typedef typename GEdgeSetTy::iterator iterator;
  typedef typename GEdgeSetTy::const_iterator const_iterator;
  ///@}

private:
  NodeID id;       ///< Node ID
  GNodeK nodeKind; ///< Node kind

  GEdgeSetTy InEdges;  ///< all incoming edge of this node
  GEdgeSetTy OutEdges; ///< all outgoing edge of this node

public:
  /// Constructor
  GenericNode(NodeID i, GNodeK k) : id(i), nodeKind(k) {}

  /// Destructor
  virtual ~GenericNode() {}

  /// Get ID
  inline NodeID getId() const { return id; }

  /// Get node kind
  inline GNodeK getNodeKind() const { return nodeKind; }

  /// Get incoming/outgoing edge set
  ///@{
  inline const GEdgeSetTy &getOutEdges() const { return OutEdges; }
  inline const GEdgeSetTy &getInEdges() const { return InEdges; }
  ///@}

  /// Has incoming/outgoing edge set
  //@{
  inline bool hasIncomingEdge() const { return (InEdges.empty() == false); }
  inline bool hasOutgoingEdge() const { return (OutEdges.empty() == false); }
  //@}

  ///  iterators
  //@{
  inline iterator OutEdgeBegin() { return OutEdges.begin(); }
  inline iterator OutEdgeEnd() { return OutEdges.end(); }
  inline iterator InEdgeBegin() { return InEdges.begin(); }
  inline iterator InEdgeEnd() { return InEdges.end(); }
  inline const_iterator OutEdgeBegin() const { return OutEdges.begin(); }
  inline const_iterator OutEdgeEnd() const { return OutEdges.end(); }
  inline const_iterator InEdgeBegin() const { return InEdges.begin(); }
  inline const_iterator InEdgeEnd() const { return InEdges.end(); }
  //@}

  /// Add incoming and outgoing edges
  //@{
  inline bool addIncomingEdge(EdgeTy *inEdge) {
    return InEdges.insert(inEdge).second;
  }
  inline bool addOutgoingEdge(EdgeTy *outEdge) {
    return OutEdges.insert(outEdge).second;
  }
  //@}

  /// Remove incoming and outgoing edges
  ///@{
  inline size_t removeIncomingEdge(EdgeTy *edge) {
    iterator it = InEdges.find(edge);
    assert(it != InEdges.end() && "can not find in edge in SVFG node");
    return InEdges.erase(edge);
  }
  inline size_t removeOutgoingEdge(EdgeTy *edge) {
    iterator it = OutEdges.find(edge);
    assert(it != OutEdges.end() && "can not find out edge in SVFG node");
    return OutEdges.erase(edge);
  }
  ///@}

  /// Find incoming and outgoing edges
  //@{
  inline EdgeTy *hasIncomingEdge(EdgeTy *edge) const {
    const_iterator it = InEdges.find(edge);
    if (it != InEdges.end())
      return *it;
    else
      return nullptr;
  }
  inline EdgeTy *hasOutgoingEdge(EdgeTy *edge) const {
    const_iterator it = OutEdges.find(edge);
    if (it != OutEdges.end())
      return *it;
    else
      return nullptr;
  }
  //@}
};

/*
 * Generic graph for program representation
 * It is base class and needs to be instantiated
 */
template <class NodeTy, class EdgeTy> class GenericGraph {

public:
  /// NodeID to GenericNode map
  typedef std::unordered_map<NodeID, NodeTy *> IDToNodeMapTy;

  /// Node Iterators
  //@{
  typedef typename IDToNodeMapTy::iterator iterator;
  typedef typename IDToNodeMapTy::const_iterator const_iterator;
  //@}

  /// Constructor
  GenericGraph() : edgeNum(0), nodeNum(0) {}

  /// Destructor
  virtual ~GenericGraph() { destroy(); }

  /// Release memory
  void destroy() {
    for (iterator I = IDToNodeMap.begin(), E = IDToNodeMap.end(); I != E; ++I)
      delete I->second;
  }
  /// Iterators
  //@{
  inline iterator begin() { return IDToNodeMap.begin(); }
  inline iterator end() { return IDToNodeMap.end(); }
  inline const_iterator begin() const { return IDToNodeMap.begin(); }
  inline const_iterator end() const { return IDToNodeMap.end(); }
  //}@

  /// Add a Node
  inline void addGNode(NodeID id, NodeTy *node) {
    IDToNodeMap[id] = node;
    nodeNum++;
  }

  /// Get a node
  inline NodeTy *getGNode(NodeID id) const {
    const_iterator it = IDToNodeMap.find(id);
    assert(it != IDToNodeMap.end() && "Node not found!");
    return it->second;
  }

  /// Has a node
  inline bool hasGNode(NodeID id) const {
    const_iterator it = IDToNodeMap.find(id);
    return it != IDToNodeMap.end();
  }

  /// Delete a node
  inline void removeGNode(NodeTy *node) {
    assert(node->hasIncomingEdge() == false &&
           node->hasOutgoingEdge() == false &&
           "node which have edges can't be deleted");
    iterator it = IDToNodeMap.find(node->getId());
    assert(it != IDToNodeMap.end() && "can not find the node");
    IDToNodeMap.erase(it);
  }

  /// Get total number of node/edge
  inline uint32_t getTotalNodeNum() const { return nodeNum; }
  inline uint32_t getTotalEdgeNum() const { return edgeNum; }
  /// Increase number of node/edge
  inline void incNodeNum() { nodeNum++; }
  inline void incEdgeNum() { edgeNum++; }

protected:
  IDToNodeMapTy IDToNodeMap; ///< node map

public:
  uint32_t edgeNum; ///< total num of node
  uint32_t nodeNum; ///< total num of edge
};
