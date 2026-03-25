#pragma once

#include "IR/SVFG/SVFG.h"
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/STLExtras.h>

namespace llvm {

struct SVFGPairToNodeRef {
  lotus::analysis::SVFGNode *
  operator()(const std::pair<const uint32_t, lotus::analysis::SVFGNode *> &it) const {
    return it.second;
  }
};

struct SVFGPairToConstNodeRef {
  const lotus::analysis::SVFGNode *
  operator()(
      const std::pair<const uint32_t, lotus::analysis::SVFGNode *> &it) const {
    return it.second;
  }
};

struct SVFGEdgeToDstNode {
  lotus::analysis::SVFGNode *
  operator()(lotus::analysis::SVFGEdge *E) const {
    return E->getDstNode();
  }
};

struct SVFGEdgeToConstDstNode {
  const lotus::analysis::SVFGNode *
  operator()(lotus::analysis::SVFGEdge *E) const {
    return static_cast<const lotus::analysis::SVFGNode *>(E->getDstNode());
  }
};

struct SVFGEdgeToSrcNode {
  lotus::analysis::SVFGNode *
  operator()(lotus::analysis::SVFGEdge *E) const {
    return E->getSrcNode();
  }
};

struct SVFGEdgeToConstSrcNode {
  const lotus::analysis::SVFGNode *
  operator()(lotus::analysis::SVFGEdge *E) const {
    return static_cast<const lotus::analysis::SVFGNode *>(E->getSrcNode());
  }
};

template <> struct GraphTraits<lotus::analysis::SVFGNode *> {
  using NodeRef = lotus::analysis::SVFGNode *;
  using EdgeIter = std::vector<lotus::analysis::SVFGEdge *>::const_iterator;
  using ChildIteratorType =
      decltype(llvm::map_iterator(EdgeIter(), SVFGEdgeToDstNode()));

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().begin(), SVFGEdgeToDstNode());
  }

  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().end(), SVFGEdgeToDstNode());
  }
};

template <> struct GraphTraits<const lotus::analysis::SVFGNode *> {
  using NodeRef = const lotus::analysis::SVFGNode *;
  using EdgeIter = std::vector<lotus::analysis::SVFGEdge *>::const_iterator;
  using ChildIteratorType =
      decltype(llvm::map_iterator(EdgeIter(), SVFGEdgeToConstDstNode()));

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().begin(), SVFGEdgeToConstDstNode());
  }

  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->getOutEdges().end(), SVFGEdgeToConstDstNode());
  }
};

template <> struct GraphTraits<lotus::analysis::SVFG *> : GraphTraits<lotus::analysis::SVFGNode *> {
  using nodes_iterator =
      decltype(llvm::map_iterator(lotus::analysis::SVFG::iterator(),
                                  SVFGPairToNodeRef()));

  static lotus::analysis::SVFGNode *getEntryNode(lotus::analysis::SVFG *G) {
    return (G->begin() == G->end()) ? nullptr : G->begin()->second;
  }

  static nodes_iterator nodes_begin(lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->begin(), SVFGPairToNodeRef());
  }

  static nodes_iterator nodes_end(lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->end(), SVFGPairToNodeRef());
  }
};

template <>
struct GraphTraits<const lotus::analysis::SVFG *>
    : GraphTraits<const lotus::analysis::SVFGNode *> {
  using nodes_iterator =
      decltype(llvm::map_iterator(lotus::analysis::SVFG::const_iterator(),
                                  SVFGPairToConstNodeRef()));

  static const lotus::analysis::SVFGNode *
  getEntryNode(const lotus::analysis::SVFG *G) {
    return (G->begin() == G->end()) ? nullptr : G->begin()->second;
  }

  static nodes_iterator nodes_begin(const lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->begin(), SVFGPairToConstNodeRef());
  }

  static nodes_iterator nodes_end(const lotus::analysis::SVFG *G) {
    return llvm::map_iterator(G->end(), SVFGPairToConstNodeRef());
  }
};

// --- Inverse GraphTraits for backward traversal ---

template <>
struct GraphTraits<Inverse<lotus::analysis::SVFGNode *>> {
  using NodeRef = lotus::analysis::SVFGNode *;
  using EdgeIter = std::vector<lotus::analysis::SVFGEdge *>::const_iterator;
  using ChildIteratorType =
      decltype(llvm::map_iterator(EdgeIter(), SVFGEdgeToSrcNode()));

  static NodeRef getEntryNode(Inverse<lotus::analysis::SVFGNode *> G) {
    return G.Graph;
  }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->getInEdges().begin(), SVFGEdgeToSrcNode());
  }

  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->getInEdges().end(), SVFGEdgeToSrcNode());
  }
};

template <>
struct GraphTraits<Inverse<const lotus::analysis::SVFGNode *>> {
  using NodeRef = const lotus::analysis::SVFGNode *;
  using EdgeIter = std::vector<lotus::analysis::SVFGEdge *>::const_iterator;
  using ChildIteratorType =
      decltype(llvm::map_iterator(EdgeIter(), SVFGEdgeToConstSrcNode()));

  static NodeRef getEntryNode(Inverse<const lotus::analysis::SVFGNode *> G) {
    return G.Graph;
  }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->getInEdges().begin(), SVFGEdgeToConstSrcNode());
  }

  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->getInEdges().end(), SVFGEdgeToConstSrcNode());
  }
};

template <>
struct GraphTraits<Inverse<lotus::analysis::SVFG *>>
    : GraphTraits<Inverse<lotus::analysis::SVFGNode *>> {
  using nodes_iterator =
      decltype(llvm::map_iterator(lotus::analysis::SVFG::iterator(),
                                  SVFGPairToNodeRef()));

  static lotus::analysis::SVFGNode *
  getEntryNode(Inverse<lotus::analysis::SVFG *> G) {
    auto *graph = G.Graph;
    return (graph->begin() == graph->end()) ? nullptr
                                            : graph->begin()->second;
  }

  static nodes_iterator nodes_begin(Inverse<lotus::analysis::SVFG *> G) {
    return llvm::map_iterator(G.Graph->begin(), SVFGPairToNodeRef());
  }

  static nodes_iterator nodes_end(Inverse<lotus::analysis::SVFG *> G) {
    return llvm::map_iterator(G.Graph->end(), SVFGPairToNodeRef());
  }
};

} // namespace llvm
