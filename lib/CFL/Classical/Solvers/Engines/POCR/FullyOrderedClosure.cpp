#include "CFL/Classical/Solvers/Engines/POCR/FullyOrderedClosure.h"

#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines {

class FullyOrderedTransitiveClosure::Impl {
public:
  Impl(std::size_t node_count, bool simplify_cycles)
      : simplify_cycles_(simplify_cycles) {
    ensureNodeCount(node_count);
  }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error(
          "Fully ordered closure node count exceeds sparse-bitvector range");
    }
    const std::size_t old_size = structural_successors_.size();
    structural_successors_.resize(node_count);
    structural_predecessors_.resize(node_count);
    successors_.resize(node_count);
    predecessors_.resize(node_count);
    closed_primary_.resize(node_count);
    critical_successors_.resize(node_count);
    critical_predecessors_.resize(node_count);
    for (NodeId node = old_size; node < node_count; ++node) {
      structural_successors_[node].set(static_cast<unsigned>(node));
      structural_predecessors_[node].set(static_cast<unsigned>(node));
    }
    statistics_.nodes = node_count;
  }

  bool addPrimaryArc(NodeId source, NodeId target) {
    requireNode(source);
    requireNode(target);
    ++statistics_.arc_insertions;
    if (hasSemanticPath(source, target)) {
      ++statistics_.duplicate_pairs;
      return false;
    }
    successors_[source].set(static_cast<unsigned>(target));
    predecessors_[target].set(static_cast<unsigned>(source));
    ++edge_count_;
    statistics_.relation_edges = edge_count_;
    return true;
  }

  std::vector<std::pair<NodeId, NodeId>> closePrimaryArc(NodeId source,
                                                         NodeId target) {
    requireNode(source);
    requireNode(target);
    if (!hasSemanticPath(source, target)) {
      throw std::logic_error("FOCR primary arc was not registered");
    }
    if (!closed_primary_[source].test_and_set(static_cast<unsigned>(target))) {
      return {};
    }

    // The ECG's structural relation is reflexive for graph maintenance; the
    // public CFL relation is non-reflexive unless a non-empty cycle exists.
    // Remember which vertices become cyclic because the ECG search naturally
    // suppresses its pre-seeded structural identity pairs.
    BitVector cycle_nodes = structural_predecessors_[source];
    cycle_nodes &= structural_successors_[target];
    new_structural_pairs_.clear();

    if (!hasStructuralPath(source, target)) {
      if (hasStructuralPath(target, source)) {
        insertBackEdge(source, target);
      } else {
        insertForwardEdge(source, target);
      }
    }

    std::vector<std::pair<NodeId, NodeId>> discovered{{source, target}};
    for (const auto &[new_source, new_target] : new_structural_pairs_) {
      reportSemanticPath(new_source, new_target, discovered);
    }
    for (unsigned node : cycle_nodes) {
      reportSemanticPath(node, node, discovered);
    }

    statistics_.relation_edges = edge_count_;
    statistics_.critical_edges = critical_edge_count_;
    return discovered;
  }

  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target) {
    if (!addPrimaryArc(source, target)) {
      return {};
    }
    return closePrimaryArc(source, target);
  }

  bool hasPath(NodeId source, NodeId target) const {
    requireNode(source);
    requireNode(target);
    return hasSemanticPath(source, target);
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

  void
  traverseCriticalSuccessors(NodeId root,
                             llvm::function_ref<bool(NodeId)> visitor) const {
    traverseCriticalGraph(root, critical_successors_, visitor);
  }

  void
  traverseCriticalPredecessors(NodeId root,
                               llvm::function_ref<bool(NodeId)> visitor) const {
    traverseCriticalGraph(root, critical_predecessors_, visitor);
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
    for (const auto *vectors :
         {&structural_successors_, &structural_predecessors_, &successors_,
          &predecessors_, &closed_primary_, &critical_successors_,
          &critical_predecessors_}) {
      bytes += vectors->capacity() * sizeof(BitVector);
      for (const BitVector &bits : *vectors) {
        bytes += bits.count() * sizeof(unsigned);
      }
    }
    return bytes;
  }

  const FullyOrderedClosureStatistics &statistics() const {
    return statistics_;
  }

private:
  using BitVector = llvm::SparseBitVector<>;

  void requireNode(NodeId node) const {
    if (node >= successors_.size()) {
      throw std::out_of_range("Fully ordered closure node is out of range");
    }
  }

  static void traverseCriticalGraph(NodeId root,
                                    const std::vector<BitVector> &adjacency,
                                    llvm::function_ref<bool(NodeId)> visitor) {
    BitVector visited;
    std::vector<NodeId> worklist;
    for (unsigned node : adjacency[root]) {
      worklist.push_back(node);
    }
    while (!worklist.empty()) {
      const NodeId node = worklist.back();
      worklist.pop_back();
      if (!visited.test_and_set(static_cast<unsigned>(node))) {
        continue;
      }
      if (visitor(node)) {
        for (unsigned next : adjacency[node]) {
          if (next != root && !visited.test(next)) {
            worklist.push_back(next);
          }
        }
      }
    }
  }

  bool hasSemanticPath(NodeId source, NodeId target) const {
    return successors_[source].test(static_cast<unsigned>(target));
  }

  bool hasStructuralPath(NodeId source, NodeId target) {
    ++statistics_.reachability_checks;
    return structural_successors_[source].test(static_cast<unsigned>(target));
  }

  void setStructuralPath(NodeId source, NodeId target) {
    if (structural_successors_[source].test_and_set(
            static_cast<unsigned>(target))) {
      structural_predecessors_[target].set(static_cast<unsigned>(source));
      new_structural_pairs_.emplace_back(source, target);
    }
  }

  void reportSemanticPath(NodeId source, NodeId target,
                          std::vector<std::pair<NodeId, NodeId>> &discovered) {
    ++statistics_.propagated_pairs;
    if (!successors_[source].test_and_set(static_cast<unsigned>(target))) {
      ++statistics_.duplicate_pairs;
      return;
    }
    predecessors_[target].set(static_cast<unsigned>(source));
    discovered.emplace_back(source, target);
    ++edge_count_;
  }

  void addCriticalEdge(NodeId source, NodeId target) {
    if (critical_successors_[source].test_and_set(
            static_cast<unsigned>(target))) {
      critical_predecessors_[target].set(static_cast<unsigned>(source));
      ++critical_edge_count_;
      ++statistics_.critical_edge_insertions;
    }
  }

  void removeCriticalEdge(NodeId source, NodeId target) {
    if (!critical_successors_[source].test(static_cast<unsigned>(target))) {
      return;
    }
    critical_successors_[source].reset(static_cast<unsigned>(target));
    critical_predecessors_[target].reset(static_cast<unsigned>(source));
    --critical_edge_count_;
    ++statistics_.critical_edge_removals;
  }

  void propagateForward(NodeId source, NodeId target) {
    std::vector<NodeId> worklist{target};
    while (!worklist.empty()) {
      const NodeId current = worklist.back();
      worklist.pop_back();
      ++statistics_.forward_search_steps;
      if (structural_successors_[source].test(static_cast<unsigned>(current))) {
        continue;
      }
      setStructuralPath(source, current);
      for (unsigned successor : critical_successors_[current]) {
        if (!structural_successors_[source].test(successor)) {
          worklist.push_back(successor);
        }
      }
    }
  }

  void searchBackward(NodeId source, NodeId target, bool in_cycle) {
    std::vector<NodeId> worklist{source};
    while (!worklist.empty()) {
      const NodeId current = worklist.back();
      worklist.pop_back();
      ++statistics_.backward_search_steps;

      if (!in_cycle) {
        std::vector<NodeId> redundant;
        for (unsigned successor : critical_successors_[current]) {
          if (successor != target && hasStructuralPath(target, successor)) {
            redundant.push_back(successor);
          }
        }
        for (NodeId successor : redundant) {
          removeCriticalEdge(current, successor);
        }
      }

      propagateForward(current, target);
      std::vector<NodeId> pending_predecessors;
      for (unsigned predecessor : critical_predecessors_[current]) {
        if (!hasStructuralPath(predecessor, target)) {
          pending_predecessors.push_back(predecessor);
        }
      }
      worklist.insert(worklist.end(), pending_predecessors.begin(),
                      pending_predecessors.end());
    }
  }

  void insertForwardEdge(NodeId source, NodeId target) {
    searchBackward(source, target, false);
    addCriticalEdge(source, target);
  }

  void insertBackEdge(NodeId source, NodeId target) {
    searchBackward(source, target, true);
    addCriticalEdge(source, target);
    if (simplify_cycles_) {
      simplifyCycle(source);
    }
  }

  void simplifyCycle(NodeId member) {
    BitVector visited;
    std::vector<NodeId> visited_stack;
    stepInto(member, visited, visited_stack);
    if (!visited_stack.empty() && visited_stack.back() != member) {
      addCriticalEdge(visited_stack.back(), member);
    }
    ++statistics_.cycle_simplifications;
  }

  void stepInto(NodeId node, BitVector &visited,
                std::vector<NodeId> &visited_stack) {
    visited.set(static_cast<unsigned>(node));
    visited_stack.push_back(node);

    std::vector<NodeId> successors_in_cycle;
    for (unsigned successor : critical_successors_[node]) {
      if (hasStructuralPath(successor, node)) {
        successors_in_cycle.push_back(successor);
      }
    }
    while (!successors_in_cycle.empty()) {
      const NodeId successor = successors_in_cycle.back();
      successors_in_cycle.pop_back();
      if (visited.test(static_cast<unsigned>(successor))) {
        removeCriticalEdge(node, successor);
        continue;
      }
      if (visited_stack.back() != node) {
        removeCriticalEdge(node, successor);
        addCriticalEdge(visited_stack.back(), successor);
      }
      stepInto(successor, visited, visited_stack);
    }
  }

  std::vector<BitVector> structural_successors_;
  std::vector<BitVector> structural_predecessors_;
  std::vector<BitVector> successors_;
  std::vector<BitVector> predecessors_;
  std::vector<BitVector> closed_primary_;
  std::vector<BitVector> critical_successors_;
  std::vector<BitVector> critical_predecessors_;
  std::vector<std::pair<NodeId, NodeId>> new_structural_pairs_;
  std::size_t edge_count_ = 0;
  std::size_t critical_edge_count_ = 0;
  FullyOrderedClosureStatistics statistics_;
  bool simplify_cycles_ = false;
};

FullyOrderedTransitiveClosure::FullyOrderedTransitiveClosure(
    std::size_t node_count, bool simplify_cycles)
    : impl_(std::make_unique<Impl>(node_count, simplify_cycles)) {}

FullyOrderedTransitiveClosure::~FullyOrderedTransitiveClosure() = default;
FullyOrderedTransitiveClosure::FullyOrderedTransitiveClosure(
    FullyOrderedTransitiveClosure &&) noexcept = default;
FullyOrderedTransitiveClosure &FullyOrderedTransitiveClosure::operator=(
    FullyOrderedTransitiveClosure &&) noexcept = default;

void FullyOrderedTransitiveClosure::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool FullyOrderedTransitiveClosure::addPrimaryArc(NodeId source,
                                                  NodeId target) {
  return impl_->addPrimaryArc(source, target);
}

std::vector<std::pair<NodeId, NodeId>>
FullyOrderedTransitiveClosure::closePrimaryArc(NodeId source, NodeId target) {
  return impl_->closePrimaryArc(source, target);
}

std::vector<std::pair<NodeId, NodeId>>
FullyOrderedTransitiveClosure::addArc(NodeId source, NodeId target) {
  return impl_->addArc(source, target);
}

bool FullyOrderedTransitiveClosure::hasPath(NodeId source,
                                            NodeId target) const {
  return impl_->hasPath(source, target);
}

void FullyOrderedTransitiveClosure::forEachSuccessor(
    NodeId source, llvm::function_ref<void(NodeId)> visitor) const {
  impl_->forEachSuccessor(source, visitor);
}

void FullyOrderedTransitiveClosure::forEachPredecessor(
    NodeId target, llvm::function_ref<void(NodeId)> visitor) const {
  impl_->forEachPredecessor(target, visitor);
}

void FullyOrderedTransitiveClosure::traverseCriticalSuccessors(
    NodeId root, llvm::function_ref<bool(NodeId)> visitor) const {
  impl_->traverseCriticalSuccessors(root, visitor);
}

void FullyOrderedTransitiveClosure::traverseCriticalPredecessors(
    NodeId root, llvm::function_ref<bool(NodeId)> visitor) const {
  impl_->traverseCriticalPredecessors(root, visitor);
}

std::vector<std::pair<NodeId, NodeId>>
FullyOrderedTransitiveClosure::edges() const {
  return impl_->edges();
}

std::size_t FullyOrderedTransitiveClosure::edgeCount() const {
  return impl_->edgeCount();
}

std::size_t FullyOrderedTransitiveClosure::estimatedPayloadBytes() const {
  return impl_->estimatedPayloadBytes();
}

const FullyOrderedClosureStatistics &
FullyOrderedTransitiveClosure::statistics() const {
  return impl_->statistics();
}

} // namespace lotus::cfl::classical::engines
