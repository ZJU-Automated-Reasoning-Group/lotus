#include "CFL/Classical/HybridForest.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace lotus::cfl::classical {

class HybridReachabilityForest::Impl {
public:
  explicit Impl(std::size_t node_count) { ensureNodeCount(node_count); }

  void ensureNodeCount(std::size_t node_count) {
    while (index_.size() < node_count) {
      const NodeId node = index_.size();
      index_.emplace_back();
      reachable_.emplace_back();
      TreeNode *root = createNode(node, node);
      (void)root;
      ++statistics_.roots;
    }
  }

  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target) {
    requireNode(source);
    requireNode(target);
    ++statistics_.arc_insertions;

    std::vector<NodeId> affected_roots;
    affected_roots.reserve(index_[source].size());
    for (const auto &[root, _] : index_[source]) {
      affected_roots.push_back(root);
    }

    std::vector<std::pair<NodeId, NodeId>> discovered;
    TreeNode *target_root = findNode(target, target);
    for (NodeId root : affected_roots) {
      TreeNode *source_node = findNode(root, source);
      reportReachable(root, target, discovered);
      meld(root, source_node, target_root, discovered);
    }
    return discovered;
  }

  bool contains(NodeId source, NodeId target) const {
    requireNode(source);
    requireNode(target);
    return index_[target].count(source) != 0;
  }

  bool hasPath(NodeId source, NodeId target) const {
    requireNode(source);
    requireNode(target);
    return reachable_[source].count(target) != 0;
  }

  std::vector<NodeId> successors(NodeId source) const {
    requireNode(source);
    return {reachable_[source].begin(), reachable_[source].end()};
  }

  std::vector<NodeId> predecessors(NodeId target) const {
    requireNode(target);
    std::vector<NodeId> result;
    for (const auto &[root, _] : index_[target]) {
      if (reachable_[root].count(target) != 0) {
        result.push_back(root);
      }
    }
    return result;
  }

  std::vector<std::pair<NodeId, NodeId>> edges() const {
    std::vector<std::pair<NodeId, NodeId>> result;
    result.reserve(edgeCount());
    for (NodeId source = 0; source < reachable_.size(); ++source) {
      for (NodeId target : reachable_[source]) {
        result.push_back({source, target});
      }
    }
    return result;
  }

  std::size_t edgeCount() const {
    std::size_t count = 0;
    for (const auto &targets : reachable_) {
      count += targets.size();
    }
    return count;
  }

  const HybridForestStatistics &statistics() const { return statistics_; }

  std::size_t approximateMemoryBytes() const {
    std::size_t bytes = sizeof(*this);
    bytes += storage_.capacity() * sizeof(std::unique_ptr<TreeNode>);
    bytes += statistics_.tree_nodes * sizeof(TreeNode);
    bytes += statistics_.tree_edges * sizeof(TreeNode *);
    bytes += index_.capacity() * sizeof(decltype(index_)::value_type);
    bytes += reachable_.capacity() * sizeof(decltype(reachable_)::value_type);
    for (const auto &entries : index_) {
      bytes += entries.size() * sizeof(std::pair<const NodeId, TreeNode *>);
    }
    for (const auto &entries : reachable_) {
      bytes += entries.size() * sizeof(NodeId);
    }
    return bytes;
  }

private:
  struct TreeNode {
    NodeId id = 0;
    std::unordered_set<TreeNode *> children;
  };

  struct MeldTask {
    TreeNode *parent = nullptr;
    const TreeNode *source = nullptr;
  };

  void requireNode(NodeId node) const {
    if (node >= index_.size()) {
      throw std::out_of_range("Hybrid forest node is out of range");
    }
  }

  TreeNode *findNode(NodeId root, NodeId node) const {
    const auto it = index_[node].find(root);
    if (it == index_[node].end()) {
      throw std::logic_error("Hybrid forest index is inconsistent");
    }
    return it->second;
  }

  TreeNode *createNode(NodeId root, NodeId node) {
    auto created = std::make_unique<TreeNode>();
    created->id = node;
    TreeNode *pointer = created.get();
    const auto [_, inserted] = index_[node].emplace(root, pointer);
    if (!inserted) {
      return nullptr;
    }
    storage_.push_back(std::move(created));
    ++statistics_.tree_nodes;
    return pointer;
  }

  void reportReachable(NodeId root, NodeId node,
                       std::vector<std::pair<NodeId, NodeId>> &discovered) {
    if (reachable_[root].insert(node).second) {
      discovered.push_back({root, node});
    }
  }

  void meld(NodeId root, TreeNode *parent, const TreeNode *source,
            std::vector<std::pair<NodeId, NodeId>> &discovered) {
    std::vector<MeldTask> worklist{{parent, source}};
    while (!worklist.empty()) {
      const MeldTask task = worklist.back();
      worklist.pop_back();
      ++statistics_.meld_operations;
      reportReachable(root, task.source->id, discovered);

      TreeNode *copy = createNode(root, task.source->id);
      if (!copy) {
        ++statistics_.duplicate_melds;
        continue;
      }

      task.parent->children.insert(copy);
      ++statistics_.tree_edges;
      for (const TreeNode *child : task.source->children) {
        worklist.push_back({copy, child});
      }
    }
  }

  std::vector<std::unordered_map<NodeId, TreeNode *>> index_;
  std::vector<std::unordered_set<NodeId>> reachable_;
  std::vector<std::unique_ptr<TreeNode>> storage_;
  HybridForestStatistics statistics_;
};

HybridReachabilityForest::HybridReachabilityForest(std::size_t node_count)
    : impl_(std::make_unique<Impl>(node_count)) {}

HybridReachabilityForest::~HybridReachabilityForest() = default;
HybridReachabilityForest::HybridReachabilityForest(
    HybridReachabilityForest &&) noexcept = default;
HybridReachabilityForest &HybridReachabilityForest::operator=(
    HybridReachabilityForest &&) noexcept = default;

void HybridReachabilityForest::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

std::vector<std::pair<NodeId, NodeId>>
HybridReachabilityForest::addArc(NodeId source, NodeId target) {
  return impl_->addArc(source, target);
}

bool HybridReachabilityForest::hasPath(NodeId source, NodeId target) const {
  return impl_->hasPath(source, target);
}

std::vector<NodeId> HybridReachabilityForest::successors(NodeId source) const {
  return impl_->successors(source);
}

std::vector<NodeId>
HybridReachabilityForest::predecessors(NodeId target) const {
  return impl_->predecessors(target);
}

std::vector<std::pair<NodeId, NodeId>> HybridReachabilityForest::edges() const {
  return impl_->edges();
}

std::size_t HybridReachabilityForest::edgeCount() const {
  return impl_->edgeCount();
}

bool HybridReachabilityForest::contains(NodeId source, NodeId target) const {
  return impl_->contains(source, target);
}

std::size_t HybridReachabilityForest::approximateMemoryBytes() const {
  return impl_->approximateMemoryBytes();
}

const HybridForestStatistics &HybridReachabilityForest::statistics() const {
  return impl_->statistics();
}

} // namespace lotus::cfl::classical
