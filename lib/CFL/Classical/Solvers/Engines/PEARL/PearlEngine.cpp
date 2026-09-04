#include "CFL/Classical/Solvers/Engines/PEARL/PearlEngine.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines {
namespace {

using BitVector = llvm::SparseBitVector<>;

bool isFullyTransitive(const BinaryRuleId &rule) {
  return rule.lhs == rule.first && rule.lhs == rule.second;
}

struct PartialKey {
  NodeId node = 0;
  SymbolId symbol = 0;
  bool right = false;

  bool operator==(const PartialKey &other) const {
    return node == other.node && symbol == other.symbol && right == other.right;
  }
};

struct PartialKeyHash {
  std::size_t operator()(const PartialKey &key) const {
    return key.node ^ (static_cast<std::size_t>(key.symbol) << 2U) ^
           (key.right ? 1U : 0U);
  }
};

using PartialMap = std::unordered_map<PartialKey, BitVector, PartialKeyHash>;

const BitVector &lookup(const PartialMap &map, const PartialKey &key) {
  static const BitVector empty;
  const auto it = map.find(key);
  return it == map.end() ? empty : it->second;
}

} // namespace

class PearlEngine::Impl {
public:
  class PropagationGraph {
  public:
    explicit PropagationGraph(std::size_t node_count = 0) {
      ensureNodeCount(node_count);
    }

    void ensureNodeCount(std::size_t node_count) {
      primary_successors_.resize(node_count);
      primary_predecessors_.resize(node_count);
      predecessors_.resize(node_count);
      successors_.resize(node_count);
    }

    struct Update {
      bool primary_inserted = false;
      std::vector<std::pair<NodeId, NodeId>> closure_edges;
    };

    Update addPrimary(NodeId source, NodeId target) {
      if (predecessors_.at(target).test(static_cast<unsigned>(source))) {
        return {};
      }
      primary_successors_.at(source).set(static_cast<unsigned>(target));
      primary_predecessors_.at(target).set(static_cast<unsigned>(source));

      BitVector sources = predecessors_.at(source);
      sources.set(static_cast<unsigned>(source));
      struct Task {
        BitVector sources;
        NodeId target = 0;
      };
      std::vector<Task> worklist;
      worklist.push_back({std::move(sources), target});
      Update update;
      update.primary_inserted = true;
      while (!worklist.empty()) {
        Task task = std::move(worklist.back());
        worklist.pop_back();
        BitVector difference = task.sources;
        difference.intersectWithComplement(predecessors_.at(task.target));
        if (difference.empty()) {
          continue;
        }
        predecessors_.at(task.target) |= difference;
        for (unsigned predecessor : difference) {
          successors_.at(predecessor).set(static_cast<unsigned>(task.target));
          update.closure_edges.emplace_back(predecessor, task.target);
        }
        for (unsigned successor : primary_successors_.at(task.target)) {
          worklist.push_back({difference, successor});
        }
      }
      return update;
    }

    const BitVector &primarySuccessors(NodeId node) const {
      return primary_successors_.at(node);
    }

    const BitVector &primaryPredecessors(NodeId node) const {
      return primary_predecessors_.at(node);
    }

  private:
    std::vector<BitVector> primary_successors_;
    std::vector<BitVector> primary_predecessors_;
    std::vector<BitVector> predecessors_;
    std::vector<BitVector> successors_;
  };

  enum class Origin {
    InputOrNonTransitive,
    PartialPropagation,
    FullClosure,
  };

  Impl(const Grammar &grammar, Relation &relation, std::size_t node_count,
       PearlOptions options)
      : grammar_(grammar), relation_(relation), node_count_(node_count),
        inverse_symbol_(grammar.symbolCount(), grammar.symbolCount()) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error("PEARL node count exceeds bitvector range");
    }
    left_guards_.resize(grammar.symbolCount());
    right_guards_.resize(grammar.symbolCount());
    left_dependents_.resize(grammar.symbolCount());
    right_dependents_.resize(grammar.symbolCount());
    full_symbols_ = grammar.transitiveSymbols();
    for (const auto &[first, second] : options.inverse_relations) {
      if (first >= grammar.symbolCount() || second >= grammar.symbolCount() ||
          first == second || inverse_symbol_[first] != grammar.symbolCount() ||
          inverse_symbol_[second] != grammar.symbolCount()) {
        throw std::invalid_argument("Invalid PEARL inverse-relation pairing");
      }
      inverse_symbol_[first] = second;
      inverse_symbol_[second] = first;
    }
    for (const auto &[first, second] : options.inverse_relations) {
      if (full_symbols_.count(first) != 0 || full_symbols_.count(second) != 0) {
        full_symbols_.insert(first);
        full_symbols_.insert(second);
      }
    }
    for (const auto &[_, rules] : grammar.binaryByFirstId()) {
      for (const BinaryRuleId &rule : rules) {
        if (isFullyTransitive(rule)) {
          continue;
        }
        if (rule.lhs == rule.first && full_symbols_.count(rule.second) != 0) {
          left_guards_[rule.lhs].push_back(rule.second);
          left_dependents_[rule.second].push_back(rule.lhs);
        }
        if (rule.lhs == rule.second && full_symbols_.count(rule.first) != 0) {
          right_guards_[rule.lhs].push_back(rule.first);
          right_dependents_[rule.first].push_back(rule.lhs);
        }
      }
    }
    for (auto *symbol_lists : {&left_guards_, &right_guards_, &left_dependents_,
                               &right_dependents_}) {
      for (auto &symbols : *symbol_lists) {
        std::sort(symbols.begin(), symbols.end());
        symbols.erase(std::unique(symbols.begin(), symbols.end()),
                      symbols.end());
      }
    }
    for (SymbolId symbol : full_symbols_) {
      propagation_graphs_.emplace(symbol, PropagationGraph(node_count));
    }
    relation_.ensureNodeCount(node_count_);
  }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error("PEARL node count exceeds bitvector range");
    }
    node_count_ = std::max(node_count_, node_count);
    relation_.ensureNodeCount(node_count_);
    for (auto &[_, graph] : propagation_graphs_) {
      graph.ensureNodeCount(node_count_);
    }
  }

  bool addEdge(SymbolId symbol, NodeId source, NodeId target) {
    return derive(symbol, source, target, Origin::InputOrNonTransitive, nullptr,
                  true);
  }

  PearlStatistics solve() {
    PearlStatistics statistics;
    for (SymbolId symbol : grammar_.nullableSymbolIds()) {
      for (NodeId node = nullable_seeded_nodes_; node < node_count_; ++node) {
        derive(symbol, node, node, Origin::InputOrNonTransitive, &statistics,
               true);
      }
    }
    nullable_seeded_nodes_ = node_count_;
    while (!non_transitive_worklist_.empty() || !partial_worklist_.empty() ||
           !full_primary_worklist_.empty()) {
      ++statistics.outer_rounds;
      solveNonTransitive(statistics);
      solvePartial(statistics);
      while (!full_primary_worklist_.empty() || !partial_worklist_.empty()) {
        solveFullyTransitive(statistics);
        solvePartial(statistics);
      }
    }
    return statistics;
  }

  bool empty() const {
    return non_transitive_worklist_.empty() && partial_worklist_.empty() &&
           full_primary_worklist_.empty();
  }

private:
  bool isPartialRule(const BinaryRuleId &rule) const {
    return (rule.lhs == rule.first && rule.lhs != rule.second &&
            full_symbols_.count(rule.second) != 0) ||
           (rule.lhs == rule.second && rule.lhs != rule.first &&
            full_symbols_.count(rule.first) != 0);
  }

  bool isTransitiveRule(const BinaryRuleId &rule) const {
    return isFullyTransitive(rule) || isPartialRule(rule);
  }

  void enqueuePartial(PartialKey key) {
    if (partial_queued_.insert(key).second) {
      partial_worklist_.push_back(key);
    }
  }

  void seedPartial(SymbolId symbol, NodeId source, NodeId target) {
    if (!left_guards_.at(symbol).empty()) {
      const PartialKey key{target, symbol, false};
      if (!lookup(partial_old_, key).test(static_cast<unsigned>(source)) &&
          partial_delta_[key].test_and_set(static_cast<unsigned>(source))) {
        enqueuePartial(key);
      }
    }
    if (!right_guards_.at(symbol).empty()) {
      const PartialKey key{source, symbol, true};
      if (!lookup(partial_old_, key).test(static_cast<unsigned>(target)) &&
          partial_delta_[key].test_and_set(static_cast<unsigned>(target))) {
        enqueuePartial(key);
      }
    }
  }

  bool derive(SymbolId symbol, NodeId source, NodeId target, Origin origin,
              PearlStatistics *statistics, bool mirror) {
    if (symbol >= grammar_.symbolCount() || source >= node_count_ ||
        target >= node_count_) {
      throw std::out_of_range("PEARL edge is out of range");
    }
    if (!relation_.add(symbol, source, target)) {
      if (statistics) {
        ++statistics->duplicate_edges;
      }
      return false;
    }
    non_transitive_worklist_.push_back({symbol, source, target});
    seedPartial(symbol, source, target);
    if (origin != Origin::FullClosure && full_symbols_.count(symbol) != 0) {
      full_primary_worklist_.push_back({symbol, source, target});
    }
    if (statistics) {
      ++statistics->derived_edges;
    }
    if (mirror && inverse_symbol_[symbol] != grammar_.symbolCount()) {
      derive(inverse_symbol_[symbol], target, source, origin, statistics,
             false);
    }
    return true;
  }

  void solveNonTransitive(PearlStatistics &statistics) {
    while (!non_transitive_worklist_.empty()) {
      const RelationEdge selected = non_transitive_worklist_.front();
      non_transitive_worklist_.pop_front();
      ++statistics.non_transitive_items;

      if (const auto unary = grammar_.unaryByRhsId().find(selected.symbol);
          unary != grammar_.unaryByRhsId().end()) {
        for (SymbolId lhs : unary->second) {
          derive(lhs, selected.source, selected.target,
                 Origin::InputOrNonTransitive, &statistics, true);
        }
      }

      if (const auto binary = grammar_.binaryByFirstId().find(selected.symbol);
          binary != grammar_.binaryByFirstId().end()) {
        for (const BinaryRuleId &rule : binary->second) {
          if (isTransitiveRule(rule)) {
            continue;
          }
          std::vector<NodeId> targets;
          relation_.forEachSuccessor(
              rule.second, selected.target,
              [&](NodeId target) { targets.push_back(target); });
          for (NodeId target : targets) {
            derive(rule.lhs, selected.source, target,
                   Origin::InputOrNonTransitive, &statistics, true);
          }
        }
      }

      if (const auto binary = grammar_.binaryBySecondId().find(selected.symbol);
          binary != grammar_.binaryBySecondId().end()) {
        for (const BinaryRuleId &rule : binary->second) {
          if (isTransitiveRule(rule)) {
            continue;
          }
          std::vector<NodeId> sources;
          relation_.forEachPredecessor(
              rule.first, selected.source,
              [&](NodeId source) { sources.push_back(source); });
          for (NodeId source : sources) {
            derive(rule.lhs, source, selected.target,
                   Origin::InputOrNonTransitive, &statistics, true);
          }
        }
      }
    }
  }

  BitVector takePartialDelta(const PartialKey &key) {
    const auto it = partial_delta_.find(key);
    if (it == partial_delta_.end()) {
      return {};
    }
    BitVector delta = std::move(it->second);
    partial_delta_.erase(it);
    BitVector difference = delta;
    difference.intersectWithComplement(lookup(partial_old_, key));
    partial_old_[key] |= difference;
    return difference;
  }

  void propagatePartialSet(SymbolId symbol, NodeId node,
                           const BitVector &values, bool right,
                           PearlStatistics &statistics) {
    const auto &guards =
        right ? right_guards_.at(symbol) : left_guards_.at(symbol);
    for (SymbolId guard : guards) {
      const PropagationGraph &graph = propagation_graphs_.at(guard);
      const BitVector &next_nodes = right ? graph.primaryPredecessors(node)
                                          : graph.primarySuccessors(node);
      for (unsigned next : next_nodes) {
        ++statistics.batch_propagations;
        for (unsigned value : values) {
          if (right) {
            derive(symbol, next, value, Origin::PartialPropagation, &statistics,
                   true);
          } else {
            derive(symbol, value, next, Origin::PartialPropagation, &statistics,
                   true);
          }
        }
      }
    }
  }

  void solvePartial(PearlStatistics &statistics) {
    while (!partial_worklist_.empty()) {
      const PartialKey key = partial_worklist_.front();
      partial_worklist_.pop_front();
      partial_queued_.erase(key);
      const BitVector delta = takePartialDelta(key);
      if (delta.empty()) {
        continue;
      }
      ++statistics.partially_transitive_nodes;
      propagatePartialSet(key.symbol, key.node, delta, key.right, statistics);
    }
  }

  BitVector completePartialSet(SymbolId symbol, NodeId node, bool right) const {
    const PartialKey key{node, symbol, right};
    BitVector result = lookup(partial_old_, key);
    result |= lookup(partial_delta_, key);
    return result;
  }

  void propagateAcrossNewPrimary(SymbolId guard, NodeId source, NodeId target,
                                 PearlStatistics &statistics) {
    for (SymbolId symbol : left_dependents_.at(guard)) {
      const BitVector values = completePartialSet(symbol, source, false);
      if (!values.empty()) {
        ++statistics.batch_propagations;
        for (unsigned value : values) {
          derive(symbol, value, target, Origin::PartialPropagation, &statistics,
                 true);
        }
      }
    }
    for (SymbolId symbol : right_dependents_.at(guard)) {
      const BitVector values = completePartialSet(symbol, target, true);
      if (!values.empty()) {
        ++statistics.batch_propagations;
        for (unsigned value : values) {
          derive(symbol, source, value, Origin::PartialPropagation, &statistics,
                 true);
        }
      }
    }
  }

  void solveFullyTransitive(PearlStatistics &statistics) {
    while (!full_primary_worklist_.empty()) {
      const RelationEdge primary = full_primary_worklist_.front();
      full_primary_worklist_.pop_front();
      PropagationGraph::Update update =
          propagation_graphs_.at(primary.symbol)
              .addPrimary(primary.source, primary.target);
      if (!update.primary_inserted) {
        continue;
      }
      ++statistics.fully_transitive_primary_edges;
      propagateAcrossNewPrimary(primary.symbol, primary.source, primary.target,
                                statistics);
      for (const auto &[source, target] : update.closure_edges) {
        const bool inserted = derive(primary.symbol, source, target,
                                     Origin::FullClosure, &statistics, true);
        statistics.fully_transitive_secondary_edges += inserted ? 1 : 0;
      }
    }
  }

  const Grammar &grammar_;
  Relation &relation_;
  std::size_t node_count_ = 0;
  std::vector<std::vector<SymbolId>> left_guards_;
  std::vector<std::vector<SymbolId>> right_guards_;
  std::vector<std::vector<SymbolId>> left_dependents_;
  std::vector<std::vector<SymbolId>> right_dependents_;
  std::unordered_set<SymbolId> full_symbols_;
  std::vector<SymbolId> inverse_symbol_;
  std::unordered_map<SymbolId, PropagationGraph> propagation_graphs_;
  std::deque<RelationEdge> non_transitive_worklist_;
  std::deque<RelationEdge> full_primary_worklist_;
  PartialMap partial_old_;
  PartialMap partial_delta_;
  std::deque<PartialKey> partial_worklist_;
  std::unordered_set<PartialKey, PartialKeyHash> partial_queued_;
  std::size_t nullable_seeded_nodes_ = 0;
};

PearlEngine::PearlEngine(const Grammar &grammar, Relation &relation,
                         std::size_t node_count, PearlOptions options)
    : impl_(std::make_unique<Impl>(grammar, relation, node_count,
                                   std::move(options))) {}

PearlEngine::~PearlEngine() = default;
PearlEngine::PearlEngine(PearlEngine &&) noexcept = default;
PearlEngine &PearlEngine::operator=(PearlEngine &&) noexcept = default;

void PearlEngine::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool PearlEngine::addEdge(SymbolId symbol, NodeId source, NodeId target) {
  return impl_->addEdge(symbol, source, target);
}

PearlStatistics PearlEngine::solve() { return impl_->solve(); }

bool PearlEngine::empty() const { return impl_->empty(); }

} // namespace lotus::cfl::classical::engines
