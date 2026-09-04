#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lotus::cfl::classical::engines {

struct PearlStatistics {
  std::size_t outer_rounds = 0;
  std::size_t non_transitive_items = 0;
  std::size_t partially_transitive_nodes = 0;
  std::size_t fully_transitive_primary_edges = 0;
  std::size_t fully_transitive_secondary_edges = 0;
  std::size_t batch_propagations = 0;
  std::size_t derived_edges = 0;
  std::size_t duplicate_edges = 0;
};

struct PearlOptions {
  /// Explicit X <-> Xbar pairs used by PackRR and UpdatePG.
  std::vector<std::pair<SymbolId, SymbolId>> inverse_relations;
};

/// ASE'23 PEARL multi-derivation engine (Algorithms 2-4).
class PearlEngine {
public:
  PearlEngine(const Grammar &grammar, Relation &relation,
              std::size_t node_count = 0, PearlOptions options = {});
  ~PearlEngine();
  PearlEngine(PearlEngine &&) noexcept;
  PearlEngine &operator=(PearlEngine &&) noexcept;
  PearlEngine(const PearlEngine &) = delete;
  PearlEngine &operator=(const PearlEngine &) = delete;

  void ensureNodeCount(std::size_t node_count);
  bool addEdge(SymbolId symbol, NodeId source, NodeId target);
  PearlStatistics solve();
  bool empty() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
