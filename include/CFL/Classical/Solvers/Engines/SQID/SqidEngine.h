#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lotus::cfl::classical::engines {

struct SqidStatistics {
  std::size_t processed_in_keys = 0;
  std::size_t processed_out_keys = 0;
  std::size_t forward_chains = 0;
  std::size_t backward_chains = 0;
  std::size_t chaining_products = 0;
  std::size_t derived_edges = 0;
  std::size_t duplicate_edges = 0;
  std::size_t peak_in_worklist = 0;
  std::size_t peak_out_worklist = 0;
};

/// OOPSLA'26 Sqid engine (Algorithms 2-4).
///
/// The engine owns the enhanced old/delta incoming/outgoing graph views while
/// materializing the complete relation in `relation` for client queries.
class SqidEngine {
public:
  SqidEngine(const Grammar &grammar, Relation &relation,
             std::size_t node_count = 0);
  ~SqidEngine();
  SqidEngine(SqidEngine &&) noexcept;
  SqidEngine &operator=(SqidEngine &&) noexcept;
  SqidEngine(const SqidEngine &) = delete;
  SqidEngine &operator=(const SqidEngine &) = delete;

  void ensureNodeCount(std::size_t node_count);
  bool addEdge(SymbolId symbol, NodeId source, NodeId target);
  SqidStatistics solve();
  bool empty() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
