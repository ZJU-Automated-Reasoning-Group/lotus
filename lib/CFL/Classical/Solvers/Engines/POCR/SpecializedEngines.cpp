#include "CFL/Classical/Solvers/Engines/POCR/SpecializedEngines.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines {
namespace {

using BitVector = llvm::SparseBitVector<>;

class BinaryRelation {
public:
  explicit BinaryRelation(std::size_t nodes = 0)
      : successors_(nodes), predecessors_(nodes) {
    if (nodes > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error("Specialized POCR graph is too large");
    }
  }

  bool add(NodeId source, NodeId target) {
    if (!successors_.at(source).test_and_set(static_cast<unsigned>(target))) {
      return false;
    }
    predecessors_.at(target).set(static_cast<unsigned>(source));
    ++edge_count_;
    return true;
  }

  bool contains(NodeId source, NodeId target) const {
    return successors_.at(source).test(static_cast<unsigned>(target));
  }

  bool remove(NodeId source, NodeId target) {
    if (!contains(source, target)) {
      return false;
    }
    successors_.at(source).reset(static_cast<unsigned>(target));
    predecessors_.at(target).reset(static_cast<unsigned>(source));
    --edge_count_;
    return true;
  }

  const BitVector &successors(NodeId source) const {
    return successors_.at(source);
  }

  const BitVector &predecessors(NodeId target) const {
    return predecessors_.at(target);
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

private:
  std::vector<BitVector> successors_;
  std::vector<BitVector> predecessors_;
  std::size_t edge_count_ = 0;
};

enum class ItemKind : std::uint8_t {
  Assignment,
  Memory,
  Value,
  Flow,
};

struct WorkItem {
  ItemKind kind = ItemKind::Assignment;
  NodeId source = 0;
  NodeId target = 0;

  bool operator==(const WorkItem &other) const {
    return kind == other.kind && source == other.source &&
           target == other.target;
  }
};

struct WorkItemHash {
  std::size_t operator()(const WorkItem &item) const {
    const std::size_t kind = static_cast<std::size_t>(item.kind);
    return item.source ^
           (item.target + (item.source << 6U) + (item.source >> 2U)) ^
           (kind << 3U);
  }
};

class UniqueQueue {
public:
  bool push(const WorkItem &item) {
    if (!present_.insert(item).second) {
      ++duplicates_;
      return false;
    }
    queue_.push_back(item);
    ++queued_;
    return true;
  }

  WorkItem pop() {
    const WorkItem item = queue_.front();
    queue_.pop_front();
    present_.erase(item);
    return item;
  }

  bool empty() const { return queue_.empty(); }
  std::size_t queued() const { return queued_; }
  std::size_t duplicates() const { return duplicates_; }

private:
  std::deque<WorkItem> queue_;
  std::unordered_set<WorkItem, WorkItemHash> present_;
  std::size_t queued_ = 0;
  std::size_t duplicates_ = 0;
};

std::pair<std::string, std::uint32_t>
splitAttributedLabel(const std::string &label) {
  const auto separator = label.find_last_of('_');
  if (separator == std::string::npos || separator + 1 == label.size()) {
    return {label, 0};
  }
  const std::string suffix = label.substr(separator + 1);
  if (suffix.find_first_not_of("0123456789") != std::string::npos) {
    return {label, 0};
  }
  const std::uint64_t attribute = std::stoull(suffix);
  if (attribute > std::numeric_limits<std::uint32_t>::max()) {
    throw std::out_of_range("POCR edge attribute exceeds uint32_t range");
  }
  return {label.substr(0, separator), static_cast<std::uint32_t>(attribute)};
}

bool isReverse(const std::string &base) {
  return base.size() >= 3 && base.compare(base.size() - 3, 3, "bar") == 0;
}

class MeldedReachabilityForest {
public:
  struct TreeNode {
    NodeId id = 0;
    std::vector<TreeNode *> children;
  };

  explicit MeldedReachabilityForest(std::size_t nodes) : index_(nodes) {
    for (NodeId node = 0; node < nodes; ++node) {
      roots_.push_back(createNode(node, node));
    }
  }

  bool contains(NodeId source, NodeId target) const {
    ++checks_;
    return index_.at(target).count(source) != 0;
  }

  TreeNode *root(NodeId node) const { return findNode(node, node); }

  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target) {
    if (contains(source, target)) {
      return {};
    }
    std::vector<NodeId> affected_roots;
    affected_roots.reserve(index_.at(source).size());
    for (const auto &[root_id, unused] : index_[source]) {
      (void)unused;
      affected_roots.push_back(root_id);
    }

    std::vector<std::pair<NodeId, NodeId>> discovered;
    TreeNode *target_root = root(target);
    for (NodeId root_id : affected_roots) {
      meld(root_id, findNode(root_id, source), target_root, discovered);
    }
    return discovered;
  }

  std::size_t edgeCount() const { return storage_.size(); }
  std::size_t checks() const { return checks_; }

  std::vector<std::pair<NodeId, NodeId>> edges() const {
    std::vector<std::pair<NodeId, NodeId>> result;
    result.reserve(storage_.size());
    for (NodeId node = 0; node < index_.size(); ++node) {
      for (const auto &[root_id, unused] : index_[node]) {
        (void)unused;
        result.emplace_back(root_id, node);
      }
    }
    return result;
  }

private:
  struct MeldTask {
    TreeNode *parent = nullptr;
    const TreeNode *source = nullptr;
  };

  TreeNode *createNode(NodeId root_id, NodeId node) {
    auto created = std::make_unique<TreeNode>();
    created->id = node;
    TreeNode *pointer = created.get();
    const auto [unused, inserted] = index_.at(node).emplace(root_id, pointer);
    (void)unused;
    if (!inserted) {
      return nullptr;
    }
    storage_.push_back(std::move(created));
    return pointer;
  }

  TreeNode *findNode(NodeId root_id, NodeId node) const {
    const auto it = index_.at(node).find(root_id);
    if (it == index_.at(node).end()) {
      throw std::logic_error("POCR hybrid-tree index is inconsistent");
    }
    return it->second;
  }

  void meld(NodeId root_id, TreeNode *parent, const TreeNode *source,
            std::vector<std::pair<NodeId, NodeId>> &discovered) {
    std::vector<MeldTask> worklist{{parent, source}};
    while (!worklist.empty()) {
      const MeldTask task = worklist.back();
      worklist.pop_back();
      TreeNode *copy = createNode(root_id, task.source->id);
      if (!copy) {
        continue;
      }
      task.parent->children.push_back(copy);
      discovered.emplace_back(root_id, task.source->id);
      for (const TreeNode *child : task.source->children) {
        worklist.push_back({copy, child});
      }
    }
  }

  std::vector<std::unordered_map<NodeId, TreeNode *>> index_;
  std::vector<TreeNode *> roots_;
  std::vector<std::unique_ptr<TreeNode>> storage_;
  mutable std::size_t checks_ = 0;
};

class EdgeCriticalGraph {
public:
  explicit EdgeCriticalGraph(std::size_t nodes, bool simplify_cycles = false)
      : reachable_(nodes), critical_(nodes), simplify_cycles_(simplify_cycles) {
    for (NodeId node = 0; node < nodes; ++node) {
      reachable_.add(node, node);
    }
  }

  bool contains(NodeId source, NodeId target) const {
    ++checks_;
    return reachable_.contains(source, target);
  }

  const BitVector &successors(NodeId node) const {
    return critical_.successors(node);
  }

  std::vector<std::pair<NodeId, NodeId>> addArc(NodeId source, NodeId target) {
    new_pairs_.clear();
    if (contains(source, target)) {
      return {};
    }
    if (contains(target, source)) {
      searchBackward(source, target, true);
      critical_.add(source, target);
      if (simplify_cycles_) {
        simplifyCycle(source);
      }
    } else {
      searchBackward(source, target, false);
      critical_.add(source, target);
    }
    return new_pairs_;
  }

  std::size_t edgeCount() const { return reachable_.edgeCount(); }
  std::size_t criticalEdgeCount() const { return critical_.edgeCount(); }
  std::size_t cycleSimplifications() const { return cycle_simplifications_; }
  std::size_t checks() const { return checks_; }
  std::vector<std::pair<NodeId, NodeId>> edges() const {
    return reachable_.edges();
  }

private:
  void setReachable(NodeId source, NodeId target) {
    if (reachable_.add(source, target)) {
      new_pairs_.emplace_back(source, target);
    }
  }

  void searchForward(NodeId source, NodeId target) {
    std::vector<NodeId> worklist{target};
    while (!worklist.empty()) {
      const NodeId current = worklist.back();
      worklist.pop_back();
      if (reachable_.contains(source, current)) {
        continue;
      }
      setReachable(source, current);
      for (unsigned successor : critical_.successors(current)) {
        if (!reachable_.contains(source, successor)) {
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
      if (!in_cycle) {
        std::vector<NodeId> redundant;
        for (unsigned successor : critical_.successors(current)) {
          if (successor != target && reachable_.contains(target, successor)) {
            redundant.push_back(successor);
          }
        }
        for (NodeId successor : redundant) {
          critical_.remove(current, successor);
        }
      }
      searchForward(current, target);
      std::vector<NodeId> predecessors;
      for (unsigned predecessor : critical_.predecessors(current)) {
        if (!reachable_.contains(predecessor, target)) {
          predecessors.push_back(predecessor);
        }
      }
      worklist.insert(worklist.end(), predecessors.begin(), predecessors.end());
    }
  }

  void simplifyCycle(NodeId member) {
    BitVector visited;
    std::vector<NodeId> visited_stack;
    stepInto(member, visited, visited_stack);
    if (!visited_stack.empty() && visited_stack.back() != member) {
      critical_.add(visited_stack.back(), member);
    }
    ++cycle_simplifications_;
  }

  void stepInto(NodeId node, BitVector &visited,
                std::vector<NodeId> &visited_stack) {
    visited.set(static_cast<unsigned>(node));
    visited_stack.push_back(node);

    std::vector<NodeId> successors_in_cycle;
    for (unsigned successor : critical_.successors(node)) {
      if (reachable_.contains(successor, node)) {
        successors_in_cycle.push_back(successor);
      }
    }
    while (!successors_in_cycle.empty()) {
      const NodeId successor = successors_in_cycle.back();
      successors_in_cycle.pop_back();
      if (visited.test(static_cast<unsigned>(successor))) {
        critical_.remove(node, successor);
        continue;
      }
      if (visited_stack.back() != node) {
        critical_.remove(node, successor);
        critical_.add(visited_stack.back(), successor);
      }
      stepInto(successor, visited, visited_stack);
    }
  }

  BinaryRelation reachable_;
  BinaryRelation critical_;
  std::vector<std::pair<NodeId, NodeId>> new_pairs_;
  mutable std::size_t checks_ = 0;
  std::size_t cycle_simplifications_ = 0;
  bool simplify_cycles_ = false;
};

bool isAliasAssignment(const std::string &base) {
  return base == "a" || base == "copy" || base == "vgep";
}

bool isAliasDereference(const std::string &base) { return base == "d"; }

bool isAliasField(const std::string &base) {
  return base == "f" || base == "gep";
}

class AliasCore {
public:
  explicit AliasCore(const LabeledGraph &graph)
      : graph_nodes_(graph.vertexCount()), assignments_(graph_nodes_),
        dereferences_(graph_nodes_), values_(graph_nodes_),
        memory_(graph_nodes_) {
    for (const LabeledEdge &edge : graph.edges()) {
      const auto [base, attribute] = splitAttributedLabel(edge.label);
      // Lotus PEG encodes object -> pointer as addr and pointer -> object as
      // addrbar. POCR's d edge is the latter dereference orientation.
      if (base == "addrbar") {
        graph_edges_ += dereferences_.add(edge.source, edge.target) ? 1 : 0;
        continue;
      }
      if (isReverse(base)) {
        continue;
      }
      if (isAliasAssignment(base)) {
        if (assignments_.add(edge.source, edge.target)) {
          ++graph_edges_;
          queue_.push({ItemKind::Assignment, edge.source, edge.target});
        }
      } else if (isAliasDereference(base)) {
        graph_edges_ += dereferences_.add(edge.source, edge.target) ? 1 : 0;
      } else if (isAliasField(base)) {
        auto [it, unused] = fields_.try_emplace(attribute, graph_nodes_);
        (void)unused;
        graph_edges_ += it->second.add(edge.source, edge.target) ? 1 : 0;
      } else if (base != "addr") {
        throw std::invalid_argument(
            "Unsupported terminal for specialized alias engine: " + edge.label);
      }
    }
    for (NodeId node = 0; node < graph_nodes_; ++node) {
      setValue(node, node);
    }
  }

  bool empty() const { return queue_.empty(); }

  WorkItem pop() {
    ++processed_items_;
    return queue_.pop();
  }

  bool setValue(NodeId source, NodeId target) {
    ++checks_;
    if (!values_.add(source, target)) {
      return false;
    }
    values_.add(target, source);
    checkDereferences(source, target);
    checkFields(source, target);
    return true;
  }

  void setMemory(NodeId source, NodeId target) {
    if (memory_.add(source, target)) {
      memory_.add(target, source);
    }
  }

  bool hasMemory(NodeId source, NodeId target) {
    ++checks_;
    return source == target || memory_.contains(source, target);
  }

  void queueValue(NodeId source, NodeId target) {
    if (queue_.push({ItemKind::Value, source, target})) {
      ++matched_pairs_;
    }
  }

  void queueAssignment(NodeId source, NodeId target) {
    queue_.push({ItemKind::Assignment, source, target});
  }

  const BitVector &valueSuccessors(NodeId source) const {
    return values_.successors(source);
  }

  bool mayAlias(NodeId first, NodeId second) const {
    return values_.contains(first, second);
  }

  std::vector<std::pair<NodeId, NodeId>> valuePairs() const {
    return values_.edges();
  }

  SpecializedPocrStatistics
  statistics(std::size_t reachability_checks, std::size_t reachability_pairs,
             std::size_t critical_edges,
             std::size_t cycle_simplifications = 0) const {
    SpecializedPocrStatistics result;
    result.graph_nodes = graph_nodes_;
    result.graph_edges = graph_edges_;
    result.processed_items = processed_items_;
    result.queued_items = queue_.queued();
    result.duplicate_items = queue_.duplicates();
    result.reachability_checks = checks_ + reachability_checks;
    result.reachability_pairs = reachability_pairs;
    result.value_or_flow_pairs = values_.edgeCount();
    result.matched_pairs = matched_pairs_;
    result.critical_edges = critical_edges;
    result.cycle_simplifications = cycle_simplifications;
    return result;
  }

private:
  void checkDereferences(NodeId source, NodeId target) {
    for (unsigned source_target : dereferences_.successors(source)) {
      for (unsigned target_target : dereferences_.successors(target)) {
        if (hasMemory(source_target, target_target)) {
          continue;
        }
        if (queue_.push({ItemKind::Memory, source_target, target_target})) {
          ++matched_pairs_;
        }
        for (unsigned predecessor : assignments_.predecessors(source_target)) {
          queueAssignment(predecessor, target_target);
        }
        for (unsigned predecessor : assignments_.predecessors(target_target)) {
          queueAssignment(predecessor, source_target);
        }
      }
    }
  }

  void checkFields(NodeId source, NodeId target) {
    for (auto &[unused, field] : fields_) {
      (void)unused;
      for (unsigned source_target : field.successors(source)) {
        for (unsigned target_target : field.successors(target)) {
          ++checks_;
          queueValue(source_target, target_target);
        }
      }
    }
  }

  std::size_t graph_nodes_ = 0;
  std::size_t graph_edges_ = 0;
  BinaryRelation assignments_;
  BinaryRelation dereferences_;
  std::map<std::uint32_t, BinaryRelation> fields_;
  BinaryRelation values_;
  BinaryRelation memory_;
  UniqueQueue queue_;
  std::size_t processed_items_ = 0;
  std::size_t checks_ = 0;
  std::size_t matched_pairs_ = 0;
};

bool isValueFlowDirect(const std::string &base) {
  return base == "a" || base == "direct" || base == "indirect" ||
         base == "thread";
}

class ValueFlowCore {
public:
  explicit ValueFlowCore(const LabeledGraph &graph)
      : graph_nodes_(graph.vertexCount()) {
    for (const LabeledEdge &edge : graph.edges()) {
      const auto [base, attribute] = splitAttributedLabel(edge.label);
      if (isReverse(base)) {
        continue;
      }
      if (isValueFlowDirect(base)) {
        if (queue_.push({ItemKind::Flow, edge.source, edge.target})) {
          ++graph_edges_;
        }
      } else if (base == "call") {
        auto [it, unused] = calls_.try_emplace(attribute, graph_nodes_);
        (void)unused;
        graph_edges_ += it->second.add(edge.source, edge.target) ? 1 : 0;
      } else if (base == "ret") {
        auto [it, unused] = returns_.try_emplace(attribute, graph_nodes_);
        (void)unused;
        graph_edges_ += it->second.add(edge.source, edge.target) ? 1 : 0;
      }
    }
  }

  void seedEmptyFlows() {
    for (NodeId node = 0; node < graph_nodes_; ++node) {
      matchCallReturn(node, node);
    }
  }

  bool empty() const { return queue_.empty(); }

  WorkItem pop() {
    ++processed_items_;
    return queue_.pop();
  }

  void matchCallReturn(NodeId source, NodeId target) {
    for (const auto &[attribute, calls] : calls_) {
      const auto returns = returns_.find(attribute);
      if (returns == returns_.end()) {
        continue;
      }
      for (unsigned call_source : calls.predecessors(source)) {
        for (unsigned return_target : returns->second.successors(target)) {
          ++checks_;
          if (queue_.push({ItemKind::Flow, call_source, return_target})) {
            ++matched_pairs_;
          }
        }
      }
    }
  }

  SpecializedPocrStatistics
  statistics(std::size_t reachability_checks, std::size_t reachability_pairs,
             std::size_t critical_edges,
             std::size_t cycle_simplifications = 0) const {
    SpecializedPocrStatistics result;
    result.graph_nodes = graph_nodes_;
    result.graph_edges = graph_edges_;
    result.processed_items = processed_items_;
    result.queued_items = queue_.queued();
    result.duplicate_items = queue_.duplicates();
    result.reachability_checks = checks_ + reachability_checks;
    result.reachability_pairs = reachability_pairs;
    result.value_or_flow_pairs = reachability_pairs;
    result.matched_pairs = matched_pairs_;
    result.critical_edges = critical_edges;
    result.cycle_simplifications = cycle_simplifications;
    return result;
  }

private:
  std::size_t graph_nodes_ = 0;
  std::size_t graph_edges_ = 0;
  std::map<std::uint32_t, BinaryRelation> calls_;
  std::map<std::uint32_t, BinaryRelation> returns_;
  UniqueQueue queue_;
  std::size_t processed_items_ = 0;
  std::size_t checks_ = 0;
  std::size_t matched_pairs_ = 0;
};

} // namespace

class PocrAliasEngine::Impl {
public:
  explicit Impl(const LabeledGraph &graph)
      : reachability_(graph.vertexCount()), core_(graph) {}

  SpecializedPocrStatistics solve() {
    while (!core_.empty()) {
      const WorkItem item = core_.pop();
      if (item.kind == ItemKind::Assignment) {
        reachability_.addArc(item.source, item.target);
        std::vector<NodeId> value_targets;
        for (unsigned target : core_.valueSuccessors(item.source)) {
          value_targets.push_back(target);
        }
        for (NodeId target : value_targets) {
          core_.queueValue(target, item.target);
        }
      } else if (item.kind == ItemKind::Memory) {
        core_.setMemory(item.source, item.target);
        addValue(reachability_.root(item.source),
                 reachability_.root(item.target));
      } else if (item.kind == ItemKind::Value) {
        addValue(reachability_.root(item.source),
                 reachability_.root(item.target));
      }
    }
    solved_ = true;
    return core_.statistics(reachability_.checks(), reachability_.edgeCount(),
                            0);
  }

  bool mayAlias(NodeId first, NodeId second) const {
    requireSolved();
    return core_.mayAlias(first, second);
  }

  bool assignmentReachable(NodeId source, NodeId target) const {
    requireSolved();
    return reachability_.contains(source, target);
  }

  std::vector<std::pair<NodeId, NodeId>> valuePairs() const {
    requireSolved();
    return core_.valuePairs();
  }

private:
  using TreeNode = MeldedReachabilityForest::TreeNode;

  void addValue(TreeNode *source, TreeNode *target) {
    std::vector<std::pair<TreeNode *, TreeNode *>> worklist{{source, target}};
    while (!worklist.empty()) {
      const auto [current_source, current_target] = worklist.back();
      worklist.pop_back();
      if (!core_.setValue(current_source->id, current_target->id)) {
        continue;
      }
      for (TreeNode *child : current_target->children) {
        worklist.emplace_back(current_source, child);
      }
      for (TreeNode *child : current_source->children) {
        worklist.emplace_back(child, current_target);
      }
    }
  }

  void requireSolved() const {
    if (!solved_) {
      throw std::logic_error("PocrAliasEngine::solve() has not been called");
    }
  }

  MeldedReachabilityForest reachability_;
  AliasCore core_;
  bool solved_ = false;
};

class FocrAliasEngine::Impl {
public:
  Impl(const LabeledGraph &graph, bool simplify_cycles)
      : reachability_(graph.vertexCount(), simplify_cycles), core_(graph) {}

  SpecializedPocrStatistics solve() {
    while (!core_.empty()) {
      const WorkItem item = core_.pop();
      if (item.kind == ItemKind::Assignment) {
        reachability_.addArc(item.source, item.target);
        std::vector<NodeId> value_targets;
        for (unsigned target : core_.valueSuccessors(item.source)) {
          value_targets.push_back(target);
        }
        for (NodeId target : value_targets) {
          core_.queueValue(target, item.target);
        }
      } else if (item.kind == ItemKind::Memory) {
        core_.setMemory(item.source, item.target);
        addValue(item.source, item.target);
      } else if (item.kind == ItemKind::Value) {
        addValue(item.source, item.target);
      }
    }
    solved_ = true;
    return core_.statistics(reachability_.checks(), reachability_.edgeCount(),
                            reachability_.criticalEdgeCount(),
                            reachability_.cycleSimplifications());
  }

  bool mayAlias(NodeId first, NodeId second) const {
    requireSolved();
    return core_.mayAlias(first, second);
  }

  bool assignmentReachable(NodeId source, NodeId target) const {
    requireSolved();
    return reachability_.contains(source, target);
  }

  std::vector<std::pair<NodeId, NodeId>> valuePairs() const {
    requireSolved();
    return core_.valuePairs();
  }

private:
  void addValue(NodeId source, NodeId target) {
    std::vector<std::pair<NodeId, NodeId>> worklist{{source, target}};
    while (!worklist.empty()) {
      const auto [current_source, current_target] = worklist.back();
      worklist.pop_back();
      if (!core_.setValue(current_source, current_target)) {
        continue;
      }
      for (unsigned successor : reachability_.successors(current_target)) {
        worklist.emplace_back(current_source, successor);
      }
      for (unsigned successor : reachability_.successors(current_source)) {
        worklist.emplace_back(successor, current_target);
      }
    }
  }

  void requireSolved() const {
    if (!solved_) {
      throw std::logic_error("FocrAliasEngine::solve() has not been called");
    }
  }

  EdgeCriticalGraph reachability_;
  AliasCore core_;
  bool solved_ = false;
};

class PocrValueFlowEngine::Impl {
public:
  explicit Impl(const LabeledGraph &graph)
      : reachability_(graph.vertexCount()), core_(graph) {
    core_.seedEmptyFlows();
  }

  SpecializedPocrStatistics solve() {
    while (!core_.empty()) {
      const WorkItem item = core_.pop();
      for (const auto &[source, target] :
           reachability_.addArc(item.source, item.target)) {
        core_.matchCallReturn(source, target);
      }
    }
    solved_ = true;
    return core_.statistics(reachability_.checks(), reachability_.edgeCount(),
                            0);
  }

  bool hasFlow(NodeId source, NodeId target) const {
    requireSolved();
    return reachability_.contains(source, target);
  }

  std::vector<std::pair<NodeId, NodeId>> flowPairs() const {
    requireSolved();
    return reachability_.edges();
  }

private:
  void requireSolved() const {
    if (!solved_) {
      throw std::logic_error(
          "PocrValueFlowEngine::solve() has not been called");
    }
  }

  MeldedReachabilityForest reachability_;
  ValueFlowCore core_;
  bool solved_ = false;
};

class FocrValueFlowEngine::Impl {
public:
  Impl(const LabeledGraph &graph, bool simplify_cycles)
      : reachability_(graph.vertexCount(), simplify_cycles), core_(graph) {
    core_.seedEmptyFlows();
  }

  SpecializedPocrStatistics solve() {
    while (!core_.empty()) {
      const WorkItem item = core_.pop();
      for (const auto &[source, target] :
           reachability_.addArc(item.source, item.target)) {
        core_.matchCallReturn(source, target);
      }
    }
    solved_ = true;
    return core_.statistics(reachability_.checks(), reachability_.edgeCount(),
                            reachability_.criticalEdgeCount(),
                            reachability_.cycleSimplifications());
  }

  bool hasFlow(NodeId source, NodeId target) const {
    requireSolved();
    return reachability_.contains(source, target);
  }

  std::vector<std::pair<NodeId, NodeId>> flowPairs() const {
    requireSolved();
    return reachability_.edges();
  }

private:
  void requireSolved() const {
    if (!solved_) {
      throw std::logic_error(
          "FocrValueFlowEngine::solve() has not been called");
    }
  }

  EdgeCriticalGraph reachability_;
  ValueFlowCore core_;
  bool solved_ = false;
};

PocrAliasEngine::PocrAliasEngine(const LabeledGraph &graph)
    : impl_(std::make_unique<Impl>(graph)) {}
PocrAliasEngine::~PocrAliasEngine() = default;
PocrAliasEngine::PocrAliasEngine(PocrAliasEngine &&) noexcept = default;
PocrAliasEngine &
PocrAliasEngine::operator=(PocrAliasEngine &&) noexcept = default;
SpecializedPocrStatistics PocrAliasEngine::solve() { return impl_->solve(); }
bool PocrAliasEngine::mayAlias(NodeId first, NodeId second) const {
  return impl_->mayAlias(first, second);
}
bool PocrAliasEngine::assignmentReachable(NodeId source, NodeId target) const {
  return impl_->assignmentReachable(source, target);
}
std::vector<std::pair<NodeId, NodeId>> PocrAliasEngine::valuePairs() const {
  return impl_->valuePairs();
}

FocrAliasEngine::FocrAliasEngine(const LabeledGraph &graph,
                                 bool simplify_cycles)
    : impl_(std::make_unique<Impl>(graph, simplify_cycles)) {}
FocrAliasEngine::~FocrAliasEngine() = default;
FocrAliasEngine::FocrAliasEngine(FocrAliasEngine &&) noexcept = default;
FocrAliasEngine &
FocrAliasEngine::operator=(FocrAliasEngine &&) noexcept = default;
SpecializedPocrStatistics FocrAliasEngine::solve() { return impl_->solve(); }
bool FocrAliasEngine::mayAlias(NodeId first, NodeId second) const {
  return impl_->mayAlias(first, second);
}
bool FocrAliasEngine::assignmentReachable(NodeId source, NodeId target) const {
  return impl_->assignmentReachable(source, target);
}
std::vector<std::pair<NodeId, NodeId>> FocrAliasEngine::valuePairs() const {
  return impl_->valuePairs();
}

PocrValueFlowEngine::PocrValueFlowEngine(const LabeledGraph &graph)
    : impl_(std::make_unique<Impl>(graph)) {}
PocrValueFlowEngine::~PocrValueFlowEngine() = default;
PocrValueFlowEngine::PocrValueFlowEngine(PocrValueFlowEngine &&) noexcept =
    default;
PocrValueFlowEngine &
PocrValueFlowEngine::operator=(PocrValueFlowEngine &&) noexcept = default;
SpecializedPocrStatistics PocrValueFlowEngine::solve() {
  return impl_->solve();
}
bool PocrValueFlowEngine::hasFlow(NodeId source, NodeId target) const {
  return impl_->hasFlow(source, target);
}
std::vector<std::pair<NodeId, NodeId>> PocrValueFlowEngine::flowPairs() const {
  return impl_->flowPairs();
}

FocrValueFlowEngine::FocrValueFlowEngine(const LabeledGraph &graph,
                                         bool simplify_cycles)
    : impl_(std::make_unique<Impl>(graph, simplify_cycles)) {}
FocrValueFlowEngine::~FocrValueFlowEngine() = default;
FocrValueFlowEngine::FocrValueFlowEngine(FocrValueFlowEngine &&) noexcept =
    default;
FocrValueFlowEngine &
FocrValueFlowEngine::operator=(FocrValueFlowEngine &&) noexcept = default;
SpecializedPocrStatistics FocrValueFlowEngine::solve() {
  return impl_->solve();
}
bool FocrValueFlowEngine::hasFlow(NodeId source, NodeId target) const {
  return impl_->hasFlow(source, target);
}
std::vector<std::pair<NodeId, NodeId>> FocrValueFlowEngine::flowPairs() const {
  return impl_->flowPairs();
}

} // namespace lotus::cfl::classical::engines
