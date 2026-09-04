#include "CFL/Classical/Solvers/Engines/TransitiveClosure.h"

#include <limits>
#include <stdexcept>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines {

class IncrementalTransitiveClosure::Impl {
public:
  explicit Impl(std::size_t node_count) { ensureNodeCount(node_count); }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error(
          "Transitive closure node count exceeds sparse-bitvector range");
    }
    successors_.resize(node_count);
    predecessors_.resize(node_count);
    statistics_.nodes = node_count;
  }

  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target) {
    requireNode(source);
    requireNode(target);
    ++statistics_.arc_insertions;

    BitVector sources = predecessors_[source];
    BitVector targets = successors_[target];
    sources.set(static_cast<unsigned>(source));
    targets.set(static_cast<unsigned>(target));

    std::vector<std::pair<NodeId, NodeId>> discovered;
    for (unsigned predecessor : sources) {
      for (unsigned successor : targets) {
        ++statistics_.propagated_pairs;
        if (successors_[predecessor].test(successor)) {
          ++statistics_.duplicate_pairs;
          continue;
        }
        successors_[predecessor].set(successor);
        predecessors_[successor].set(predecessor);
        discovered.emplace_back(predecessor, successor);
        ++edge_count_;
      }
    }
    statistics_.relation_edges = edge_count_;
    return discovered;
  }

  bool hasPath(NodeId source, NodeId target) const {
    requireNode(source);
    requireNode(target);
    return successors_[source].test(static_cast<unsigned>(target));
  }

  void forEachSuccessor(NodeId source,
                        llvm::function_ref<void(NodeId)> visitor) const {
    requireNode(source);
    for (unsigned target : successors_[source]) {
      visitor(target);
    }
  }

  void forEachPredecessor(NodeId target,
                          llvm::function_ref<void(NodeId)> visitor) const {
    requireNode(target);
    for (unsigned source : predecessors_[target]) {
      visitor(source);
    }
  }

  std::vector<std::pair<NodeId, NodeId>> edges() const {
    std::vector<std::pair<NodeId, NodeId>> result;
    result.reserve(edge_count_);
    for (NodeId source = 0; source < successors_.size(); ++source) {
      for (unsigned target : successors_[source]) {
        result.emplace_back(source, target);
      }
    }
    return result;
  }

  std::size_t edgeCount() const { return edge_count_; }

  std::size_t estimatedPayloadBytes() const {
    std::size_t bytes = sizeof(*this);
    bytes +=
        (successors_.capacity() + predecessors_.capacity()) * sizeof(BitVector);
    for (const BitVector &bits : successors_) {
      bytes += bits.count() * sizeof(unsigned);
    }
    for (const BitVector &bits : predecessors_) {
      bytes += bits.count() * sizeof(unsigned);
    }
    return bytes;
  }

  const TransitiveClosureStatistics &statistics() const { return statistics_; }

private:
  using BitVector = llvm::SparseBitVector<>;

  void requireNode(NodeId node) const {
    if (node >= successors_.size()) {
      throw std::out_of_range("Transitive closure node is out of range");
    }
  }

  std::vector<BitVector> successors_;
  std::vector<BitVector> predecessors_;
  std::size_t edge_count_ = 0;
  TransitiveClosureStatistics statistics_;
};

IncrementalTransitiveClosure::IncrementalTransitiveClosure(
    std::size_t node_count)
    : impl_(std::make_unique<Impl>(node_count)) {}

IncrementalTransitiveClosure::~IncrementalTransitiveClosure() = default;
IncrementalTransitiveClosure::IncrementalTransitiveClosure(
    IncrementalTransitiveClosure &&) noexcept = default;
IncrementalTransitiveClosure &IncrementalTransitiveClosure::operator=(
    IncrementalTransitiveClosure &&) noexcept = default;

void IncrementalTransitiveClosure::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

std::vector<std::pair<NodeId, NodeId>>
IncrementalTransitiveClosure::addArc(NodeId source, NodeId target) {
  return impl_->addArc(source, target);
}

bool IncrementalTransitiveClosure::hasPath(NodeId source, NodeId target) const {
  return impl_->hasPath(source, target);
}

void IncrementalTransitiveClosure::forEachSuccessor(
    NodeId source, llvm::function_ref<void(NodeId)> visitor) const {
  impl_->forEachSuccessor(source, visitor);
}

void IncrementalTransitiveClosure::forEachPredecessor(
    NodeId target, llvm::function_ref<void(NodeId)> visitor) const {
  impl_->forEachPredecessor(target, visitor);
}

std::vector<std::pair<NodeId, NodeId>>
IncrementalTransitiveClosure::edges() const {
  return impl_->edges();
}

std::size_t IncrementalTransitiveClosure::edgeCount() const {
  return impl_->edgeCount();
}

std::size_t IncrementalTransitiveClosure::estimatedPayloadBytes() const {
  return impl_->estimatedPayloadBytes();
}

const TransitiveClosureStatistics &
IncrementalTransitiveClosure::statistics() const {
  return impl_->statistics();
}

} // namespace lotus::cfl::classical::engines
