#pragma once

#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Graph/ConstraintGraph/ConstraintGraph.h"
#include "CFL/Classical/Alias.h"

#include <string>

namespace lotus::cfl::classical {

/// Convert Lotus's AserPTA constraint graph into the solver-neutral alias IR.
/// Offset constraints are conservatively represented as variant GEP edges;
/// clients with field-offset metadata may refine them to NormalGep afterward.
template <typename Context>
AliasConstraintGraph
encodeAserConstraintGraph(const aser::ConstraintGraph<Context> &source) {
  AliasConstraintGraph result;
  for (const auto *node : source) {
    result.addNode(std::to_string(node->getNodeID()));
  }

  for (const auto *node : source) {
    for (auto it = node->succ_edge_begin(); it != node->succ_edge_end(); ++it) {
      const auto edge = *it;
      AliasConstraintEdgeKind kind = AliasConstraintEdgeKind::Copy;
      switch (edge.first) {
      case aser::Constraints::load:
        kind = AliasConstraintEdgeKind::Load;
        break;
      case aser::Constraints::store:
        kind = AliasConstraintEdgeKind::Store;
        break;
      case aser::Constraints::copy:
        kind = AliasConstraintEdgeKind::Copy;
        break;
      case aser::Constraints::addr_of:
        kind = AliasConstraintEdgeKind::Addr;
        break;
      case aser::Constraints::offset:
        kind = AliasConstraintEdgeKind::VariantGep;
        break;
      }
      result.addEdge(node->getNodeID(), edge.second->getNodeID(), kind);
    }
  }
  return result;
}

} // namespace lotus::cfl::classical
