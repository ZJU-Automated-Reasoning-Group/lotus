#pragma once

#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/ADT/STLFunctionalExtras.h>

namespace lotus::cfl::classical::engines {

struct FullyOrderedClosureStatistics {
  std::size_t nodes = 0;
  std::size_t relation_edges = 0;
  std::size_t arc_insertions = 0;
  std::size_t propagated_pairs = 0;
  std::size_t duplicate_pairs = 0;
  std::size_t reachability_checks = 0;
  std::size_t critical_edges = 0;
  std::size_t critical_edge_insertions = 0;
  std::size_t critical_edge_removals = 0;
  std::size_t forward_search_steps = 0;
  std::size_t backward_search_steps = 0;
  std::size_t cycle_simplifications = 0;
};

/// FOCR's edge-critical-graph incremental transitive closure.
///
/// The critical graph retains only an ordered reachability skeleton. Forward
/// and backward searches update that skeleton while sparse bitvectors expose
/// the complete non-empty-path relation required by the CFL solver.
class FullyOrderedTransitiveClosure {
public:
  explicit FullyOrderedTransitiveClosure(std::size_t node_count = 0,
                                         bool simplify_cycles = false);
  ~FullyOrderedTransitiveClosure();
  FullyOrderedTransitiveClosure(FullyOrderedTransitiveClosure &&) noexcept;
  FullyOrderedTransitiveClosure &
  operator=(FullyOrderedTransitiveClosure &&) noexcept;
  FullyOrderedTransitiveClosure(const FullyOrderedTransitiveClosure &) = delete;
  FullyOrderedTransitiveClosure &
  operator=(const FullyOrderedTransitiveClosure &) = delete;

  void ensureNodeCount(std::size_t node_count);
  bool addPrimaryArc(NodeId source, NodeId target);
  std::vector<std::pair<NodeId, NodeId>> closePrimaryArc(NodeId source,
                                                         NodeId target);
  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target);
  bool hasPath(NodeId source, NodeId target) const;
  void forEachSuccessor(NodeId source,
                        llvm::function_ref<void(NodeId)> visitor) const;
  void forEachPredecessor(NodeId target,
                          llvm::function_ref<void(NodeId)> visitor) const;
  void
  traverseCriticalSuccessors(NodeId root,
                             llvm::function_ref<bool(NodeId)> visitor) const;
  void
  traverseCriticalPredecessors(NodeId root,
                               llvm::function_ref<bool(NodeId)> visitor) const;
  std::vector<std::pair<NodeId, NodeId>> edges() const;
  std::size_t edgeCount() const;
  std::size_t estimatedPayloadBytes() const;
  const FullyOrderedClosureStatistics &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
