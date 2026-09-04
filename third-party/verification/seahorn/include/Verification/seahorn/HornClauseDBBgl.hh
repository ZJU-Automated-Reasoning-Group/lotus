#ifndef _HCDB_BOOST_GRAPH_TRAITS_HPP__
#define _HCDB_BOOST_GRAPH_TRAITS_HPP__

/* View of a HornClauseDB as a BGL graph */

#include "seahorn/Expr/Expr.hh"
#include "seahorn/HornClauseDB.hh"

#include <boost/graph/graph_traits.hpp>
#include <boost/graph/properties.hpp>
#include <boost/iterator/transform_iterator.hpp>

namespace seahorn {
namespace bgl {
using EEPair = std::pair<expr::Expr, expr::Expr>; // fdecl -> fdecl

struct MkOutEdgePair {
  using argument_type = expr::Expr;
  using result_type = EEPair;

  expr::Expr src;
  MkOutEdgePair() : src(NULL) {}
  MkOutEdgePair(expr::Expr u) : src(u) { assert(expr::op::bind::isFdecl(u)); }

  EEPair operator()(expr::Expr dst) const {
    assert(src);
    assert(expr::op::bind::isFdecl(dst));
    return std::make_pair(src, dst);
  }
};

struct MkInEdgePair {
  using argument_type = expr::Expr;
  using result_type = EEPair;

  expr::Expr dst;

  MkInEdgePair() : dst(NULL) {}
  MkInEdgePair(expr::Expr v) : dst(v) { assert(expr::op::bind::isFdecl(dst)); }
  EEPair operator()(expr::Expr src) const {
    assert(dst);
    assert(expr::op::bind::isFdecl(src));
    return std::make_pair(src, dst);
  }
};

} // namespace bgl
} // namespace seahorn

namespace boost {
template <> struct graph_traits<seahorn::HornClauseDBCallGraph> {
  using vertex_descriptor = expr::Expr; // fdecl
  using edge_descriptor = seahorn::bgl::EEPair;

  using edge_parallel_category = disallow_parallel_edge_tag;
  using directed_category = bidirectional_tag;
  struct this_graph_tag : virtual bidirectional_graph_tag,
                          virtual vertex_list_graph_tag {};
  using traversal_category = this_graph_tag;

  using vertices_size_type = size_t;
  using edges_size_type = size_t;
  using degree_size_type = size_t;

  using out_edge_iterator = boost::transform_iterator<
      seahorn::bgl::MkOutEdgePair,
      seahorn::HornClauseDB::expr_set_type::const_iterator>;

  using in_edge_iterator = boost::transform_iterator<
      seahorn::bgl::MkInEdgePair,
      seahorn::HornClauseDB::expr_set_type::const_iterator>;

  using vertex_iterator = seahorn::HornClauseDB::expr_set_type::const_iterator;

  /** unimplemented iterator over edges to make filtered_graph happy */
  using edge_iterator = in_edge_iterator;

  static vertex_descriptor null_vertex() { return NULL; }
};

inline expr::Expr source(const seahorn::bgl::EEPair e,
                         const seahorn::HornClauseDBCallGraph &callgraph) {
  return e.first;
}

inline expr::Expr target(const seahorn::bgl::EEPair e,
                         const seahorn::HornClauseDBCallGraph &callgraph) {
  return e.second;
}

namespace {
using out_eit =
    typename graph_traits<seahorn::HornClauseDBCallGraph>::out_edge_iterator;
using in_eit =
    typename graph_traits<seahorn::HornClauseDBCallGraph>::in_edge_iterator;
using vit =
    typename graph_traits<seahorn::HornClauseDBCallGraph>::vertex_iterator;
} // namespace

inline std::pair<out_eit, out_eit>
out_edges(expr::Expr e, const seahorn::HornClauseDBCallGraph &callgraph) {
  auto const &callees = callgraph.callees(e);
  return std::make_pair(
      make_transform_iterator(callees.begin(), seahorn::bgl::MkOutEdgePair(e)),
      make_transform_iterator(callees.end(), seahorn::bgl::MkOutEdgePair(e)));
}

inline size_t out_degree(expr::Expr e,
                         const seahorn::HornClauseDBCallGraph &callgraph) {
  return callgraph.callees(e).size();
}

inline std::pair<in_eit, in_eit>
in_edges(expr::Expr e, const seahorn::HornClauseDBCallGraph &callgraph) {
  auto const &callers = callgraph.callers(e);
  return std::make_pair(
      make_transform_iterator(callers.begin(), seahorn::bgl::MkInEdgePair(e)),
      make_transform_iterator(callers.end(), seahorn::bgl::MkInEdgePair(e)));
}

inline size_t in_degree(expr::Expr e,
                        const seahorn::HornClauseDBCallGraph &callgraph) {
  return callgraph.callers(e).size();
}

inline size_t degree(expr::Expr e,
                     const seahorn::HornClauseDBCallGraph &callgraph) {
  return callgraph.callees(e).size() + callgraph.callers(e).size();
}

inline std::pair<vit, vit>
vertices(const seahorn::HornClauseDBCallGraph &callgraph) {
  return std::make_pair(callgraph.db().getRelations().begin(),
                        callgraph.db().getRelations().end());
}

inline size_t num_vertices(const seahorn::HornClauseDBCallGraph &callgraph) {
  return callgraph.db().getRelations().size();
}

} // namespace boost
#endif /*  _HCDB_BOOST_GRAPH_TRAITS_HPP__ */
