#pragma once
/// An adapter between llvm::GraphTraits and boost::graph_traits

#include "llvm/ADT/GraphTraits.h"

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>
#include <boost/iterator/transform_iterator.hpp>

namespace seahorn {

namespace graph {
template <typename T> struct llvm_null_trait {};

template <typename T> struct llvm_null_trait<T *> {
  static inline T *null_value() { return NULL; }
};

template <typename G>
struct MkOutEdge {
  using Node = typename boost::graph_traits<G>::vertex_descriptor;
  using Edge = typename boost::graph_traits<G>::edge_descriptor;
  using argument_type = Node;
  using result_type = Edge;

  Node m_src;

  MkOutEdge() {}
  MkOutEdge(Node &src) : m_src(src) {}

  Edge operator()(Node &dst) const { return Edge(m_src, dst); }
};

template <typename G>
struct MkInEdge {
  using Node = typename boost::graph_traits<G>::vertex_descriptor;
  using Edge = typename boost::graph_traits<G>::edge_descriptor;
  using argument_type = Node;
  using result_type = Edge;

  Node m_dst;

  MkInEdge() {}
  MkInEdge(Node &dst) : m_dst(dst) {}

  Edge operator()(Node &src) const { return Edge(src, m_dst); }
};
} // namespace graph

} // namespace seahorn

namespace boost {
template <typename Graph> struct graph_traits<Graph *> {
  using GraphPtr = Graph *;
  using llvm_graph = llvm::GraphTraits<GraphPtr>;
  using llvm_succ_iterator = typename llvm_graph::ChildIteratorType;

  using llvm_inverse_graph = llvm::GraphTraits<llvm::Inverse<GraphPtr>>;
  using llvm_pred_iterator = typename llvm_inverse_graph::ChildIteratorType;

  using vertex_descriptor = typename llvm_graph::NodeType *;
  using edge_descriptor = typename std::pair<vertex_descriptor, vertex_descriptor>;

  using const_edge_descriptor = std::pair<const vertex_descriptor, const vertex_descriptor>;

  using edge_parallel_category = disallow_parallel_edge_tag;
  using directed_category = bidirectional_tag;
  struct this_graph_tag : virtual bidirectional_graph_tag,
                          virtual vertex_list_graph_tag {};
  using traversal_category = this_graph_tag;

  using vertices_size_type = size_t;
  using edges_size_type = size_t;
  using degree_size_type = size_t;

  using out_edge_iterator = boost::transform_iterator<seahorn::graph::MkOutEdge<GraphPtr>,
                                    llvm_succ_iterator>;

  using in_edge_iterator = boost::transform_iterator<seahorn::graph::MkInEdge<GraphPtr>,
                                    llvm_pred_iterator>;

  using vertex_iterator = typename llvm_graph::nodes_iterator;

  /** unimplemented iterator over edges to make filtered_graph happy */
  using edge_iterator = in_edge_iterator;

  static vertex_descriptor null_vertex() {
    return seahorn::graph::llvm_null_trait<vertex_descriptor>::null_value();
  }
};

template <typename G>
typename graph_traits<G *>::vertex_descriptor
source(typename graph_traits<G *>::edge_descriptor e, G *g) {
  return e.first;
}

template <typename G>
typename graph_traits<G *>::vertex_descriptor
target(typename graph_traits<G *>::edge_descriptor e, G *g) {
  return e.second;
}

template <typename G>
std::pair<typename graph_traits<G *>::out_edge_iterator,
          typename graph_traits<G *>::out_edge_iterator>
out_edges(typename graph_traits<G *>::vertex_descriptor v, G *g) {
  return std::make_pair(
      make_transform_iterator(llvm::GraphTraits<G *>::child_begin(v),
                              seahorn::graph::MkOutEdge<G *>(v)),
      make_transform_iterator(llvm::GraphTraits<G *>::child_end(v),
                              seahorn::graph::MkOutEdge<G *>(v)));
}

template <typename G>
size_t out_degree(typename graph_traits<G *>::vertex_descriptor v, G *g) {
  return std::distance(llvm::GraphTraits<G *>::child_begin(v),
                       llvm::GraphTraits<G *>::child_end(v));
}

template <typename G>
std::pair<typename graph_traits<G *>::in_edge_iterator,
          typename graph_traits<G *>::in_edge_iterator>
in_edges(typename graph_traits<G *>::vertex_descriptor v, G *g) {
  return std::make_pair(
      make_transform_iterator(
          llvm::GraphTraits<llvm::Inverse<G *>>::child_begin(v),
          seahorn::graph::MkInEdge<G *>(v)),
      make_transform_iterator(
          llvm::GraphTraits<llvm::Inverse<G *>>::child_end(v),
          seahorn::graph::MkInEdge<G *>(v)));
}

template <typename G>
size_t in_degree(typename graph_traits<G *>::vertex_descriptor v, G *g) {
  return std::distance(llvm::GraphTraits<llvm::Inverse<G *>>::child_begin(v),
                       llvm::GraphTraits<llvm::Inverse<G *>>::child_end(v));
}

template <typename G>
size_t degree(typename graph_traits<G *>::vertex_descriptor v, G *g) {
  return out_degree(v, g) + in_degree(v, g);
}

template <typename G>
std::pair<typename graph_traits<G *>::vertex_iterator,
          typename graph_traits<G *>::vertex_iterator>
vertices(G *g) {
  return std::make_pair(llvm::GraphTraits<G *>::nodes_begin(g),
                        llvm::GraphTraits<G *>::nodes_end(g));
}

template <typename G> size_t num_vertices(G *g) {
  return std::distance(llvm::GraphTraits<G *>::nodes_begin(g),
                       llvm::GraphTraits<G *>::nodes_end(g));
}

} // namespace boost
