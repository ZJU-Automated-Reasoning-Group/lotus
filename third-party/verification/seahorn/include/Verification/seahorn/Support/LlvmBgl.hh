#ifndef __LLVM_BGL_HPP_
#define __LLVM_BGL_HPP_
/** BGL interface to LLVM CFG */

#include "llvm/IR/CFG.h"

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>
#include <boost/iterator/transform_iterator.hpp>
#include <boost/property_map/property_map.hpp>

namespace llvm {
using BBPair = std::pair<BasicBlock *, BasicBlock *>;
namespace bgl {
struct MkOutEdgePair {
  using argument_type = BasicBlock *;
  using result_type = BBPair;

  BasicBlock *src;
  MkOutEdgePair() : src(NULL) {}
  MkOutEdgePair(BasicBlock *u) : src(u) {}

  BBPair operator()(BasicBlock *v) const {
    assert(src);
    return std::make_pair(src, v);
  }
};

struct MkInEdgePair {
  using argument_type = BasicBlock *;
  using result_type = BBPair;

  BasicBlock *dst;

  MkInEdgePair() : dst(NULL) {}

  MkInEdgePair(BasicBlock *v) : dst(v) {}
  BBPair operator()(BasicBlock *u) const {
    assert(dst);
    return std::make_pair(u, dst);
  }
};

} // namespace bgl
} // namespace llvm

namespace boost {
template <> struct graph_traits<llvm::Function> {
  using vertex_descriptor = llvm::BasicBlock *;
  using edge_descriptor = llvm::BBPair;

  using edge_parallel_category = disallow_parallel_edge_tag;
  using directed_category = bidirectional_tag;
  struct this_graph_tag : virtual bidirectional_graph_tag,
                          virtual vertex_list_graph_tag {};
  using traversal_category = this_graph_tag;

  using vertices_size_type = size_t;
  using edges_size_type = size_t;
  using degree_size_type = size_t;

  using out_edge_iterator = boost::transform_iterator<llvm::bgl::MkOutEdgePair,
                                    llvm::succ_iterator>;

  using in_edge_iterator = boost::transform_iterator<llvm::bgl::MkInEdgePair,
                                    llvm::pred_iterator>;

  using vertex_iterator = llvm::Function::const_iterator;

  /** unimplemented iterator over edges to make filtered_graph happy */
  using edge_iterator = in_edge_iterator;

  static vertex_descriptor null_vertex() { return NULL; }
};

inline llvm::BasicBlock *source(const llvm::BBPair e, const llvm::Function &f) {
  return e.first;
}

inline llvm::BasicBlock *target(const llvm::BBPair e, const llvm::Function &f) {
  return e.second;
}
} // namespace boost

namespace llvm {
namespace bgl {
using out_eit = typename boost::graph_traits<::llvm::Function>::out_edge_iterator;
using in_eit = typename boost::graph_traits<::llvm::Function>::in_edge_iterator;
using vit = llvm::Function::const_iterator;
} // namespace bgl
} // namespace llvm

namespace boost {
inline std::pair<llvm::bgl::out_eit, llvm::bgl::out_eit>
out_edges(llvm::BasicBlock *bb, const llvm::Function &f) {
  return std::make_pair(
      make_transform_iterator(succ_begin(bb), llvm::bgl::MkOutEdgePair(bb)),
      make_transform_iterator(succ_end(bb), llvm::bgl::MkOutEdgePair(bb)));
}

inline size_t out_degree(const llvm::BasicBlock *bb, const llvm::Function &f) {
  return bb->getTerminator()->getNumSuccessors();
}

inline std::pair<llvm::bgl::in_eit, llvm::bgl::in_eit>
in_edges(llvm::BasicBlock *bb, const llvm::Function &f) {
  return std::make_pair(
      make_transform_iterator(pred_begin(bb), llvm::bgl::MkInEdgePair(bb)),
      make_transform_iterator(pred_end(bb), llvm::bgl::MkInEdgePair(bb)));
}

inline size_t in_degree(const llvm::BasicBlock *bb, const llvm::Function &f) {
  return bb->getNumUses();
}

inline size_t degree(const llvm::BasicBlock *bb, const llvm::Function &f) {
  return bb->getNumUses() + bb->getTerminator()->getNumSuccessors();
}

inline std::pair<llvm::bgl::vit, llvm::bgl::vit>
vertices(const llvm::Function &f) {
  return std::make_pair(f.begin(), f.end());
}

inline size_t num_vertices(const llvm::Function &f) { return f.size(); }

} // namespace boost

#endif
