#include "CFL/Classical/Solvers/Engines/POCR/PairedTreeClosure.h"

#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines {

class PocrTransitiveClosure::Impl {
public:
  explicit Impl(std::size_t node_count) { ensureNodeCount(node_count); }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error(
          "POCR closure node count exceeds sparse-bitvector range");
    }
    successors_.resize(node_count);
    predecessors_.resize(node_count);
    closed_primary_.resize(node_count);
    predecessor_forest_.ensureNodeCount(node_count);
    successor_forest_.ensureNodeCount(node_count);
    statistics_.nodes = node_count;
    statistics_.tree_roots =
        predecessor_forest_.rootCount() + successor_forest_.rootCount();
    updateTreeStatistics();
  }

  bool addPrimaryArc(NodeId source, NodeId target) {
    requireNode(source);
    requireNode(target);
    ++statistics_.arc_insertions;
    if (successors_[source].test(static_cast<unsigned>(target))) {
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
    if (!successors_[source].test(static_cast<unsigned>(target))) {
      throw std::logic_error("POCR primary arc was not registered");
    }
    if (!closed_primary_[source].test_and_set(static_cast<unsigned>(target))) {
      return {};
    }

    BitVector affected_sources = predecessors_[source];
    BitVector affected_targets = successors_[target];
    affected_sources.set(static_cast<unsigned>(source));
    affected_targets.set(static_cast<unsigned>(target));

    std::vector<std::pair<NodeId, NodeId>> discovered{{source, target}};
    std::vector<TraversalTask> worklist;
    worklist.push_back({TraversalKind::SuccessorEnter, target, source,
                        predecessor_forest_.node(source, source),
                        successor_forest_.node(target, target)});

    while (!worklist.empty()) {
      const TraversalTask task = worklist.back();
      worklist.pop_back();
      ++statistics_.traversal_steps;

      switch (task.kind) {
      case TraversalKind::SuccessorEnter:
        // Visit predecessor combinations before inspecting the (possibly
        // extended) successor children, matching POCR's traversal order.
        worklist.push_back({TraversalKind::SuccessorChildren,
                            task.predecessor_parent, task.successor_parent,
                            task.predecessor, task.successor});
        worklist.push_back({TraversalKind::Predecessor, task.predecessor_parent,
                            task.successor_parent, task.predecessor,
                            task.successor});
        break;

      case TraversalKind::SuccessorChildren:
        for (TreeNode *child : task.successor->children) {
          if (!successor_forest_.contains(task.predecessor->id, child->id)) {
            worklist.push_back({TraversalKind::SuccessorEnter,
                                task.predecessor_parent, task.successor->id,
                                task.predecessor, child});
          }
        }
        break;

      case TraversalKind::Predecessor:
        updatePair(task.predecessor_parent, task.predecessor,
                   task.successor_parent, task.successor, discovered);
        for (TreeNode *child : task.predecessor->children) {
          if (!predecessor_forest_.contains(task.successor->id, child->id)) {
            worklist.push_back({TraversalKind::Predecessor,
                                task.predecessor->id, task.successor_parent,
                                child, task.successor});
          }
        }
        break;
      }
    }

    // Tree roots encode structural identity, whereas a CFL transitive symbol
    // denotes non-empty paths unless it is independently nullable. In a cycle,
    // POCR's membership pruning therefore skips newly semantic reflexive pairs.
    // Report exactly those reflexive cycle pairs without replacing POCR's tree
    // traversal with a predecessor/successor Cartesian product.
    affected_sources &= affected_targets;
    for (unsigned node : affected_sources) {
      if (!successors_[node].test(node)) {
        reportPair(node, node, discovered);
      }
    }

    statistics_.relation_edges = edge_count_;
    updateTreeStatistics();
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

  void traverseSuccessorTree(NodeId root,
                             llvm::function_ref<bool(NodeId)> visitor) const {
    traverseTree(successor_forest_.node(root, root), visitor);
  }

  void traversePredecessorTree(NodeId root,
                               llvm::function_ref<bool(NodeId)> visitor) const {
    traverseTree(predecessor_forest_.node(root, root), visitor);
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
    bytes += (successors_.capacity() + predecessors_.capacity() +
              closed_primary_.capacity()) *
             sizeof(BitVector);
    for (const BitVector &bits : successors_) {
      bytes += bits.count() * sizeof(unsigned);
    }
    for (const BitVector &bits : predecessors_) {
      bytes += bits.count() * sizeof(unsigned);
    }
    for (const BitVector &bits : closed_primary_) {
      bytes += bits.count() * sizeof(unsigned);
    }
    bytes += predecessor_forest_.estimatedPayloadBytes();
    bytes += successor_forest_.estimatedPayloadBytes();
    return bytes;
  }

  const PocrClosureStatistics &statistics() const { return statistics_; }

private:
  using BitVector = llvm::SparseBitVector<>;

  struct TreeNode {
    NodeId id = 0;
    std::vector<TreeNode *> children;
  };

  class ReachabilityForest {
  public:
    void ensureNodeCount(std::size_t node_count) {
      while (index_.size() < node_count) {
        const NodeId root = index_.size();
        index_.emplace_back();
        roots_.push_back(createNode(root, root));
      }
    }

    bool contains(NodeId root, NodeId node_id) const {
      return index_.at(node_id).count(root) != 0;
    }

    TreeNode *node(NodeId root, NodeId node_id) const {
      const auto &entries = index_.at(node_id);
      const auto it = entries.find(root);
      if (it == entries.end()) {
        throw std::logic_error("POCR tree index is inconsistent");
      }
      return it->second;
    }

    TreeNode *insert(NodeId root, NodeId node_id, TreeNode *parent) {
      TreeNode *created = createNode(root, node_id);
      if (!created) {
        return nullptr;
      }
      if (!parent) {
        throw std::logic_error("POCR tree insertion has no parent");
      }
      parent->children.push_back(created);
      ++tree_edges_;
      return created;
    }

    std::size_t rootCount() const { return roots_.size(); }
    std::size_t nodeCount() const { return storage_.size(); }
    std::size_t edgeCount() const { return tree_edges_; }

    std::size_t estimatedPayloadBytes() const {
      std::size_t bytes = sizeof(*this);
      bytes += roots_.capacity() * sizeof(TreeNode *);
      bytes += storage_.capacity() * sizeof(std::unique_ptr<TreeNode>);
      bytes += index_.capacity() * sizeof(decltype(index_)::value_type);
      for (const auto &entries : index_) {
        bytes += entries.size() * sizeof(std::pair<const NodeId, TreeNode *>);
      }
      for (const auto &entry : storage_) {
        bytes += sizeof(TreeNode);
        bytes += entry->children.capacity() * sizeof(TreeNode *);
      }
      return bytes;
    }

  private:
    TreeNode *createNode(NodeId root, NodeId node_id) {
      auto created = std::make_unique<TreeNode>();
      created->id = node_id;
      TreeNode *pointer = created.get();
      const auto [_, inserted] = index_.at(node_id).emplace(root, pointer);
      if (!inserted) {
        return nullptr;
      }
      storage_.push_back(std::move(created));
      return pointer;
    }

    std::vector<std::unordered_map<NodeId, TreeNode *>> index_;
    std::vector<TreeNode *> roots_;
    std::vector<std::unique_ptr<TreeNode>> storage_;
    std::size_t tree_edges_ = 0;
  };

  enum class TraversalKind {
    SuccessorEnter,
    SuccessorChildren,
    Predecessor,
  };

  struct TraversalTask {
    TraversalKind kind = TraversalKind::SuccessorEnter;
    NodeId predecessor_parent = 0;
    NodeId successor_parent = 0;
    TreeNode *predecessor = nullptr;
    TreeNode *successor = nullptr;
  };

  void requireNode(NodeId node_id) const {
    if (node_id >= successors_.size()) {
      throw std::out_of_range("POCR closure node is out of range");
    }
  }

  static void traverseTree(const TreeNode *root,
                           llvm::function_ref<bool(NodeId)> visitor) {
    std::vector<const TreeNode *> worklist(root->children.begin(),
                                           root->children.end());
    while (!worklist.empty()) {
      const TreeNode *node = worklist.back();
      worklist.pop_back();
      if (visitor(node->id)) {
        worklist.insert(worklist.end(), node->children.begin(),
                        node->children.end());
      }
    }
  }

  void reportPair(NodeId source, NodeId target,
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

  void updatePair(NodeId predecessor_parent, TreeNode *predecessor,
                  NodeId successor_parent, TreeNode *successor,
                  std::vector<std::pair<NodeId, NodeId>> &discovered) {
    reportPair(predecessor->id, successor->id, discovered);

    TreeNode *predecessor_tree_parent =
        predecessor_forest_.node(successor->id, predecessor_parent);
    TreeNode *new_predecessor = predecessor_forest_.insert(
        successor->id, predecessor->id, predecessor_tree_parent);
    if (!new_predecessor) {
      return;
    }

    TreeNode *successor_tree_parent =
        successor_forest_.node(predecessor->id, successor_parent);
    TreeNode *new_successor = successor_forest_.insert(
        predecessor->id, successor->id, successor_tree_parent);
    if (!new_successor) {
      throw std::logic_error("POCR predecessor/successor trees diverged");
    }
  }

  void updateTreeStatistics() {
    statistics_.tree_nodes =
        predecessor_forest_.nodeCount() + successor_forest_.nodeCount();
    statistics_.tree_edges =
        predecessor_forest_.edgeCount() + successor_forest_.edgeCount();
  }

  std::vector<BitVector> successors_;
  std::vector<BitVector> predecessors_;
  std::vector<BitVector> closed_primary_;
  ReachabilityForest predecessor_forest_;
  ReachabilityForest successor_forest_;
  std::size_t edge_count_ = 0;
  PocrClosureStatistics statistics_;
};

PocrTransitiveClosure::PocrTransitiveClosure(std::size_t node_count)
    : impl_(std::make_unique<Impl>(node_count)) {}

PocrTransitiveClosure::~PocrTransitiveClosure() = default;
PocrTransitiveClosure::PocrTransitiveClosure(
    PocrTransitiveClosure &&) noexcept = default;
PocrTransitiveClosure &
PocrTransitiveClosure::operator=(PocrTransitiveClosure &&) noexcept = default;

void PocrTransitiveClosure::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool PocrTransitiveClosure::addPrimaryArc(NodeId source, NodeId target) {
  return impl_->addPrimaryArc(source, target);
}

std::vector<std::pair<NodeId, NodeId>>
PocrTransitiveClosure::closePrimaryArc(NodeId source, NodeId target) {
  return impl_->closePrimaryArc(source, target);
}

std::vector<std::pair<NodeId, NodeId>>
PocrTransitiveClosure::addArc(NodeId source, NodeId target) {
  return impl_->addArc(source, target);
}

bool PocrTransitiveClosure::hasPath(NodeId source, NodeId target) const {
  return impl_->hasPath(source, target);
}

void PocrTransitiveClosure::forEachSuccessor(
    NodeId source, llvm::function_ref<void(NodeId)> visitor) const {
  impl_->forEachSuccessor(source, visitor);
}

void PocrTransitiveClosure::forEachPredecessor(
    NodeId target, llvm::function_ref<void(NodeId)> visitor) const {
  impl_->forEachPredecessor(target, visitor);
}

void PocrTransitiveClosure::traverseSuccessorTree(
    NodeId root, llvm::function_ref<bool(NodeId)> visitor) const {
  impl_->traverseSuccessorTree(root, visitor);
}

void PocrTransitiveClosure::traversePredecessorTree(
    NodeId root, llvm::function_ref<bool(NodeId)> visitor) const {
  impl_->traversePredecessorTree(root, visitor);
}

std::vector<std::pair<NodeId, NodeId>> PocrTransitiveClosure::edges() const {
  return impl_->edges();
}

std::size_t PocrTransitiveClosure::edgeCount() const {
  return impl_->edgeCount();
}

std::size_t PocrTransitiveClosure::estimatedPayloadBytes() const {
  return impl_->estimatedPayloadBytes();
}

const PocrClosureStatistics &PocrTransitiveClosure::statistics() const {
  return impl_->statistics();
}

} // namespace lotus::cfl::classical::engines
