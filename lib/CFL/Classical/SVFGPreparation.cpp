#include "CFL/Classical/SVFGPreparation.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <vector>

namespace lotus::cfl::classical {
namespace {

bool isStrongUpdateStore(const lotus::analysis::SVFG &svfg,
                         const lotus::analysis::StoreSVFGNode &store) {
  const auto &points_to = store.getMemoryPointsTo();
  if (points_to.size() != 1) {
    return false;
  }
  const std::uint32_t object = *points_to.begin();
  const auto *info = svfg.getObjectInfo(object);
  return info && info->isSingleton && !info->isHeap && !info->isArray &&
         !info->isFieldInsensitive && !info->isUnknown;
}

bool isDereferenceInput(const lotus::analysis::SVFGEdge &edge,
                        const lotus::analysis::SVFGNode &node) {
  if (const auto *store =
          llvm::dyn_cast<lotus::analysis::StoreSVFGNode>(&node)) {
    return edge.getSrcNode()->getId() == store->getStoreToPtr() &&
           (edge.isStoreEdge() || edge.isDirectEdge());
  }
  if (const auto *load = llvm::dyn_cast<lotus::analysis::LoadSVFGNode>(&node)) {
    return edge.getSrcNode()->getId() == load->getLoadFromPtr() &&
           (edge.isLoadEdge() || edge.isDirectEdge());
  }
  return false;
}

} // namespace

SVFGPreparationStatistics
prepareSVFGForCFL(lotus::analysis::SVFG &svfg,
                  const SVFGPreparationOptions &options) {
  SVFGPreparationStatistics statistics;
  for (const auto &[_, node] : svfg) {
    if (!node) {
      continue;
    }
    const auto *store = llvm::dyn_cast<lotus::analysis::StoreSVFGNode>(node);
    const bool strong_update = store && options.prune_strong_update_inputs &&
                               isStrongUpdateStore(svfg, *store);
    if (store) {
      ++statistics.stores_examined;
      statistics.strong_update_stores += strong_update ? 1 : 0;
    }

    std::vector<lotus::analysis::SVFGEdge *> remove;
    for (lotus::analysis::SVFGEdge *edge : node->getInEdges()) {
      if (!edge) {
        continue;
      }
      if (options.remove_dereference_direct_edges &&
          isDereferenceInput(*edge, *node)) {
        remove.push_back(edge);
        ++statistics.dereference_edges_removed;
      } else if (strong_update && edge->isIndirectEdge()) {
        remove.push_back(edge);
        ++statistics.strong_update_edges_removed;
      }
    }
    for (lotus::analysis::SVFGEdge *edge : remove) {
      svfg.removeEdge(edge);
    }
  }
  return statistics;
}

} // namespace lotus::cfl::classical
