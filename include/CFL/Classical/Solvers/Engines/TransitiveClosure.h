#pragma once

#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/ADT/STLFunctionalExtras.h>

namespace lotus::cfl::classical::engines {

struct TransitiveClosureStatistics {
  std::size_t nodes = 0;
  std::size_t relation_edges = 0;
  std::size_t arc_insertions = 0;
  std::size_t propagated_pairs = 0;
  std::size_t duplicate_pairs = 0;
};

/// Incremental transitive closure backed by forward and reverse sparse
/// bitvectors. Adding u -> v crosses predecessors(u) with successors(v) and
/// returns exactly the newly discovered reachability pairs.
class IncrementalTransitiveClosure {
public:
  explicit IncrementalTransitiveClosure(std::size_t node_count = 0);
  ~IncrementalTransitiveClosure();
  IncrementalTransitiveClosure(IncrementalTransitiveClosure &&) noexcept;
  IncrementalTransitiveClosure &
  operator=(IncrementalTransitiveClosure &&) noexcept;
  IncrementalTransitiveClosure(const IncrementalTransitiveClosure &) = delete;
  IncrementalTransitiveClosure &
  operator=(const IncrementalTransitiveClosure &) = delete;

  void ensureNodeCount(std::size_t node_count);
  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target);
  bool hasPath(NodeId source, NodeId target) const;
  void forEachSuccessor(NodeId source,
                        llvm::function_ref<void(NodeId)> visitor) const;
  void forEachPredecessor(NodeId target,
                          llvm::function_ref<void(NodeId)> visitor) const;
  std::vector<std::pair<NodeId, NodeId>> edges() const;
  std::size_t edgeCount() const;
  std::size_t estimatedPayloadBytes() const;
  const TransitiveClosureStatistics &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
