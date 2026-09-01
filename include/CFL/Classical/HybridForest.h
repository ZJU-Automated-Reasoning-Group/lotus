#pragma once

#include "CFL/Classical/Relation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {

struct HybridForestStatistics {
  std::size_t roots = 0;
  std::size_t tree_nodes = 0;
  std::size_t tree_edges = 0;
  std::size_t arc_insertions = 0;
  std::size_t meld_operations = 0;
  std::size_t duplicate_melds = 0;
};

/// Incremental transitive-reachability forest.
///
/// Every graph node owns a reachability tree. A node v appears at most once in
/// the tree rooted at u, so membership of v in that tree represents u ->* v.
/// Adding an arc u -> v melds v's tree into every tree that already contains
/// u and returns exactly the newly discovered reachability pairs.
class HybridReachabilityForest {
public:
  explicit HybridReachabilityForest(std::size_t node_count = 0);
  ~HybridReachabilityForest();
  HybridReachabilityForest(HybridReachabilityForest &&) noexcept;
  HybridReachabilityForest &operator=(HybridReachabilityForest &&) noexcept;
  HybridReachabilityForest(const HybridReachabilityForest &) = delete;
  HybridReachabilityForest &
  operator=(const HybridReachabilityForest &) = delete;

  void ensureNodeCount(std::size_t node_count);
  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target);
  bool hasPath(NodeId source, NodeId target) const;
  std::vector<NodeId> successors(NodeId source) const;
  std::vector<NodeId> predecessors(NodeId target) const;
  std::vector<std::pair<NodeId, NodeId>> edges() const;
  std::size_t edgeCount() const;
  bool contains(NodeId source, NodeId target) const;
  std::size_t approximateMemoryBytes() const;
  const HybridForestStatistics &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical
