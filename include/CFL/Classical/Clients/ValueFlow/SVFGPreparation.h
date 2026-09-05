#pragma once

#include <cstddef>

namespace lotus::analysis {
class SVFG;
} // namespace lotus::analysis

namespace lotus::cfl::classical {

struct SVFGPreparationOptions {
  bool remove_dereference_direct_edges = true;
  /// Remove stale incoming memory flow only with positive singleton and full
  /// overwrite evidence. Loop/recursive stack objects remain weak updates.
  bool prune_strong_update_inputs = true;
};

struct SVFGPreparationStatistics {
  std::size_t stores_examined = 0;
  std::size_t strong_update_stores = 0;
  std::size_t dereference_edges_removed = 0;
  std::size_t strong_update_edges_removed = 0;
};

SVFGPreparationStatistics
prepareSVFGForCFL(lotus::analysis::SVFG &svfg,
                  const SVFGPreparationOptions &options = {});

} // namespace lotus::cfl::classical
