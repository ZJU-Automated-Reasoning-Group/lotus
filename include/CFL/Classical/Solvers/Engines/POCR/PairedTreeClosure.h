#pragma once

#include "CFL/Classical/Core/Relation.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <llvm/ADT/STLFunctionalExtras.h>

namespace lotus::cfl::classical::engines {

struct PocrClosureStatistics {
  std::size_t nodes = 0;
  std::size_t relation_edges = 0;
  std::size_t arc_insertions = 0;
  std::size_t propagated_pairs = 0;
  std::size_t duplicate_pairs = 0;
  std::size_t tree_roots = 0;
  std::size_t tree_nodes = 0;
  std::size_t tree_edges = 0;
  std::size_t traversal_steps = 0;
};

/// POCR's paired predecessor/successor reachability-tree representation.
///
/// Each graph node roots one predecessor tree and one successor tree. Adding
/// an arc traverses only the affected tree fragments, materializes every new
/// transitive pair once, and returns those pairs to the grammar worklist.
class PocrTransitiveClosure {
public:
  explicit PocrTransitiveClosure(std::size_t node_count = 0);
  ~PocrTransitiveClosure();
  PocrTransitiveClosure(PocrTransitiveClosure &&) noexcept;
  PocrTransitiveClosure &operator=(PocrTransitiveClosure &&) noexcept;
  PocrTransitiveClosure(const PocrTransitiveClosure &) = delete;
  PocrTransitiveClosure &operator=(const PocrTransitiveClosure &) = delete;

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
  void traverseSuccessorTree(NodeId root,
                             llvm::function_ref<bool(NodeId)> visitor) const;
  void traversePredecessorTree(NodeId root,
                               llvm::function_ref<bool(NodeId)> visitor) const;
  std::vector<std::pair<NodeId, NodeId>> edges() const;
  std::size_t edgeCount() const;
  std::size_t estimatedPayloadBytes() const;
  const PocrClosureStatistics &statistics() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lotus::cfl::classical::engines
