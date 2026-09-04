#include "CFL/Classical/Solvers/Engines/SQID/SqidEngine.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines {
namespace {

using BitVector = llvm::SparseBitVector<>;

struct RelationKey {
  NodeId node = 0;
  SymbolId symbol = 0;

  bool operator==(const RelationKey &other) const {
    return node == other.node && symbol == other.symbol;
  }
};

struct RelationKeyHash {
  std::size_t operator()(const RelationKey &key) const {
    return key.node ^ (static_cast<std::size_t>(key.symbol) + (key.node << 6U) +
                       (key.node >> 2U));
  }
};

using DirectionalMap =
    std::unordered_map<RelationKey, BitVector, RelationKeyHash>;

const BitVector &lookup(const DirectionalMap &map, RelationKey key) {
  static const BitVector empty;
  const auto it = map.find(key);
  return it == map.end() ? empty : it->second;
}

} // namespace

class SqidEngine::Impl {
public:
  Impl(const Grammar &grammar, Relation &relation, std::size_t node_count)
      : grammar_(grammar), relation_(relation), node_count_(node_count) {
    ensureNodeCount(node_count);
  }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error("Sqid node count exceeds bitvector range");
    }
    node_count_ = std::max(node_count_, node_count);
    relation_.ensureNodeCount(node_count_);
  }

  bool addEdge(SymbolId symbol, NodeId source, NodeId target) {
    return deriveEdge(symbol, source, target, nullptr, false);
  }

  SqidStatistics solve() {
    SqidStatistics statistics;
    for (SymbolId symbol : grammar_.nullableSymbolIds()) {
      for (NodeId node = nullable_seeded_nodes_; node < node_count_; ++node) {
        deriveEdge(symbol, node, node, &statistics, false);
      }
    }
    nullable_seeded_nodes_ = node_count_;
    statistics.peak_in_worklist = in_worklist_.size();
    statistics.peak_out_worklist = out_worklist_.size();

    while (!in_worklist_.empty() || !out_worklist_.empty()) {
      while (!in_worklist_.empty()) {
        const RelationKey key = poll(in_worklist_, queued_in_);
        const BitVector sources = flush(key, delta_in_, old_in_);
        if (sources.empty()) {
          continue;
        }
        ++statistics.processed_in_keys;

        if (const auto unary = grammar_.unaryByRhsId().find(key.symbol);
            unary != grammar_.unaryByRhsId().end()) {
          for (SymbolId lhs : unary->second) {
            deriveIncoming(lhs, sources, key.node, statistics);
          }
        }

        if (const auto binary = grammar_.binaryByFirstId().find(key.symbol);
            binary != grammar_.binaryByFirstId().end()) {
          for (const BinaryRuleId &rule : binary->second) {
            adaptiveChain(sources, lookup(old_out_, {key.node, rule.second}),
                          rule.lhs, statistics);
          }
        }
      }

      while (!out_worklist_.empty()) {
        const RelationKey key = poll(out_worklist_, queued_out_);
        const BitVector targets = flush(key, delta_out_, old_out_);
        if (targets.empty()) {
          continue;
        }
        ++statistics.processed_out_keys;

        if (const auto binary = grammar_.binaryBySecondId().find(key.symbol);
            binary != grammar_.binaryBySecondId().end()) {
          for (const BinaryRuleId &rule : binary->second) {
            adaptiveChain(lookup(old_in_, {key.node, rule.first}), targets,
                          rule.lhs, statistics);
          }
        }
      }
    }
    return statistics;
  }

  bool empty() const { return in_worklist_.empty() && out_worklist_.empty(); }

private:
  static RelationKey
  poll(std::deque<RelationKey> &worklist,
       std::unordered_set<RelationKey, RelationKeyHash> &queued) {
    const RelationKey key = worklist.front();
    worklist.pop_front();
    queued.erase(key);
    return key;
  }

  static BitVector flush(RelationKey key, DirectionalMap &delta,
                         DirectionalMap &old) {
    const auto it = delta.find(key);
    if (it == delta.end()) {
      return {};
    }
    BitVector result = std::move(it->second);
    delta.erase(it);
    old[key] |= result;
    return result;
  }

  void require(SymbolId symbol, NodeId source, NodeId target) const {
    if (symbol >= grammar_.symbolCount() || source >= node_count_ ||
        target >= node_count_) {
      throw std::out_of_range("Sqid edge is out of range");
    }
  }

  void enqueue(RelationKey key, std::deque<RelationKey> &worklist,
               std::unordered_set<RelationKey, RelationKeyHash> &queued) {
    if (queued.insert(key).second) {
      worklist.push_back(key);
    }
  }

  bool deriveEdge(SymbolId symbol, NodeId source, NodeId target,
                  SqidStatistics *statistics, bool derive_out) {
    require(symbol, source, target);
    const RelationKey in_key{target, symbol};
    const RelationKey out_key{source, symbol};
    const bool exists =
        derive_out
            ? lookup(old_out_, out_key).test(static_cast<unsigned>(target)) ||
                  lookup(delta_out_, out_key)
                      .test(static_cast<unsigned>(target))
            : lookup(old_in_, in_key).test(static_cast<unsigned>(source)) ||
                  lookup(delta_in_, in_key).test(static_cast<unsigned>(source));
    if (exists) {
      if (statistics) {
        ++statistics->duplicate_edges;
      }
      return false;
    }

    delta_in_[in_key].set(static_cast<unsigned>(source));
    delta_out_[out_key].set(static_cast<unsigned>(target));
    enqueue(in_key, in_worklist_, queued_in_);
    enqueue(out_key, out_worklist_, queued_out_);
    if (!relation_.add(symbol, source, target)) {
      throw std::logic_error("Sqid enhanced relation diverged from output");
    }
    if (statistics) {
      ++statistics->derived_edges;
      statistics->peak_in_worklist =
          std::max(statistics->peak_in_worklist, in_worklist_.size());
      statistics->peak_out_worklist =
          std::max(statistics->peak_out_worklist, out_worklist_.size());
    }
    return true;
  }

  void deriveIncoming(SymbolId symbol, const BitVector &sources, NodeId target,
                      SqidStatistics &statistics) {
    for (unsigned source : sources) {
      deriveEdge(symbol, source, target, &statistics, false);
    }
  }

  void deriveOutgoing(SymbolId symbol, NodeId source, const BitVector &targets,
                      SqidStatistics &statistics) {
    for (unsigned target : targets) {
      deriveEdge(symbol, source, target, &statistics, true);
    }
  }

  void adaptiveChain(const BitVector &sources, const BitVector &targets,
                     SymbolId lhs, SqidStatistics &statistics) {
    if (sources.empty() || targets.empty()) {
      return;
    }
    statistics.chaining_products += sources.count() * targets.count();
    if (sources.count() >= targets.count()) {
      statistics.backward_chains += targets.count();
      for (unsigned target : targets) {
        deriveIncoming(lhs, sources, target, statistics);
      }
    } else {
      statistics.forward_chains += sources.count();
      for (unsigned source : sources) {
        deriveOutgoing(lhs, source, targets, statistics);
      }
    }
  }

  const Grammar &grammar_;
  Relation &relation_;
  std::size_t node_count_ = 0;
  DirectionalMap old_in_;
  DirectionalMap old_out_;
  DirectionalMap delta_in_;
  DirectionalMap delta_out_;
  std::deque<RelationKey> in_worklist_;
  std::deque<RelationKey> out_worklist_;
  std::unordered_set<RelationKey, RelationKeyHash> queued_in_;
  std::unordered_set<RelationKey, RelationKeyHash> queued_out_;
  std::size_t nullable_seeded_nodes_ = 0;
};

SqidEngine::SqidEngine(const Grammar &grammar, Relation &relation,
                       std::size_t node_count)
    : impl_(std::make_unique<Impl>(grammar, relation, node_count)) {}

SqidEngine::~SqidEngine() = default;
SqidEngine::SqidEngine(SqidEngine &&) noexcept = default;
SqidEngine &SqidEngine::operator=(SqidEngine &&) noexcept = default;

void SqidEngine::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool SqidEngine::addEdge(SymbolId symbol, NodeId source, NodeId target) {
  return impl_->addEdge(symbol, source, target);
}

SqidStatistics SqidEngine::solve() { return impl_->solve(); }

bool SqidEngine::empty() const { return impl_->empty(); }

} // namespace lotus::cfl::classical::engines
