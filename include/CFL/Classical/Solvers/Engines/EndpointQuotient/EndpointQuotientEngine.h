#pragma once

#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lotus::cfl::classical::engines {

/// Exact fixed-point statistics reported by the endpoint-quotient engine.
/// `logical_facts` counts concrete facts (seed plus inferred) without
/// expanding compressed rectangles; `cells` counts the occupied quotient
/// cells. The timing fields split preprocessing (partitions, lifts, bridges),
/// saturation, and exact counting, matching the underlying GEQ solver.
struct EndpointQuotientStatistics {
  std::size_t cells = 0;
  std::size_t logical_facts = 0;
  std::size_t seed_facts = 0;
  std::size_t inferred_facts = 0;
  std::size_t binary_joins = 0;
  std::size_t bridge_pairs = 0;
  std::size_t source_classes = 0;
  std::size_t target_classes = 0;
  std::size_t nullable_symbols = 0;
  std::size_t worklist_pops = 0;
  std::size_t peak_worklist = 0;
  std::size_t derived_facts = 0;
  std::size_t duplicate_facts = 0;
  std::uint64_t preprocess_us = 0;
  std::uint64_t saturation_us = 0;
  std::uint64_t count_us = 0;
};

/// Grammar-indexed endpoint-quotient (GEQ) engine adapter.
///
/// The engine buffers terminal input edges and, on `solve()`, builds a dense
/// `endpoint::Problem` from the normalized grammar and the buffered edges,
/// computes the exact least fixed point with the endpoint-quotient solver, and
/// materializes every derived fact (including symbolic nullable diagonals)
/// into the supplied relation. Input edges are deduplicated so repeated
/// terminal insertions return false, matching the other session backends.
///
/// The underlying quotient is static: adding a terminal edge followed by
/// another `solve()` rebuilds the problem and re-derives the fixed point from
/// scratch, rather than incrementally refining the previous partitions.
class EndpointQuotientEngine {
public:
  EndpointQuotientEngine(const Grammar &grammar, Relation &relation,
                         std::size_t node_count);
  ~EndpointQuotientEngine();
  EndpointQuotientEngine(const EndpointQuotientEngine &) = delete;
  EndpointQuotientEngine &operator=(const EndpointQuotientEngine &) = delete;

  void ensureNodeCount(std::size_t node_count);
  /// Records a terminal input edge. Returns false when the edge was already
  /// buffered for the current session.
  bool addEdge(SymbolId symbol, NodeId source, NodeId target);
  /// Recomputes the full least fixed point and materializes it into the
  /// relation. Idempotent with respect to the relation contents.
  EndpointQuotientStatistics solve();
  const EndpointQuotientStatistics &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines