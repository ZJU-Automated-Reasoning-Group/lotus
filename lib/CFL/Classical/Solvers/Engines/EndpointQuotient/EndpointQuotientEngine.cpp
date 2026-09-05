#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientEngine.h"
#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace lotus::cfl::classical::engines {
namespace {

using endpoint::Id;

struct EdgeKey {
  SymbolId symbol;
  NodeId source;
  NodeId target;

  bool operator==(const EdgeKey &other) const {
    return symbol == other.symbol && source == other.source &&
           target == other.target;
  }
};

struct EdgeKeyHash {
  std::size_t operator()(const EdgeKey &key) const noexcept {
    std::size_t hash = std::hash<SymbolId>{}(key.symbol);
    hash ^= std::hash<NodeId>{}(key.source) + 0x9e3779b9U + (hash << 6) +
            (hash >> 2);
    hash ^= std::hash<NodeId>{}(key.target) + 0x9e3779b9U + (hash << 6) +
            (hash >> 2);
    return hash;
  }
};

} // namespace

class EndpointQuotientEngine::Impl {
public:
  Impl(const Grammar &grammar, Relation &relation, std::size_t node_count)
      : grammar_(grammar), relation_(relation), node_count_(node_count) {}

  void ensureNodeCount(std::size_t node_count) {
    node_count_ = std::max(node_count_, node_count);
  }

  bool addEdge(SymbolId symbol, NodeId source, NodeId target) {
    return edges_.emplace(EdgeKey{symbol, source, target}).second;
  }

  EndpointQuotientStatistics solve() {
    endpoint::Problem problem;
    problem.nodes = node_count_;
    problem.symbols = grammar_.symbolCount();
    buildRules(problem);
    problem.edges.reserve(edges_.size());
    for (const EdgeKey &edge : edges_) {
      problem.edges.push_back({edge.source, edge.symbol, edge.target});
    }

    endpoint::Solver solver(std::move(problem));
    solver.solve();

    EndpointQuotientStatistics result = collect(solver.statistics());
    solver.forEachFact([&](Id symbol, Id source, Id target) {
      if (relation_.add(static_cast<SymbolId>(symbol), source, target)) {
        ++result.derived_facts;
      } else {
        ++result.duplicate_facts;
      }
    });
    stats_ = result;
    return result;
  }

  const EndpointQuotientStatistics &statistics() const { return stats_; }

private:
  void buildRules(endpoint::Problem &problem) const {
    for (const auto &[head, rules] : grammar_.productions()) {
      const Id lhs = grammar_.symbolId(head);
      for (const auto &rule : rules) {
        if (rule.empty()) {
          throw std::logic_error(
              "Endpoint quotient requires a normalized grammar");
        }
        if (rule.size() == 1 &&
            rule.front() == Grammar::kEpsilonSymbol) {
          problem.rules.push_back(endpoint::Rule::epsilon(lhs));
          continue;
        }
        if (rule.size() == 1) {
          problem.rules.push_back(
              endpoint::Rule::unary(lhs, grammar_.symbolId(rule.front())));
          continue;
        }
        if (rule.size() == 2) {
          problem.rules.push_back(endpoint::Rule::binary(
              lhs, grammar_.symbolId(rule[0]), grammar_.symbolId(rule[1])));
          continue;
        }
        throw std::logic_error(
            "Endpoint quotient requires a normalized (binarized) grammar");
      }
    }
  }

  static EndpointQuotientStatistics collect(const endpoint::Statistics &eq) {
    EndpointQuotientStatistics result;
    result.cells = eq.cells;
    result.logical_facts = eq.logical_facts;
    result.seed_facts = eq.seed_facts;
    result.inferred_facts = eq.inferred_facts;
    result.binary_joins = eq.binary_joins;
    result.bridge_pairs = eq.bridge_pairs;
    result.nullable_symbols = eq.nullable_symbols;
    result.worklist_pops = eq.worklist_pops;
    result.peak_worklist = eq.peak_worklist;
    for (const auto &symbol : eq.per_symbol) {
      result.source_classes += symbol.source_classes;
      result.target_classes += symbol.target_classes;
    }
    result.preprocess_us = static_cast<std::uint64_t>(eq.preprocess_ms * 1000);
    result.saturation_us = static_cast<std::uint64_t>(eq.saturation_ms * 1000);
    result.count_us = static_cast<std::uint64_t>(eq.count_ms * 1000);
    return result;
  }

  const Grammar &grammar_;
  Relation &relation_;
  std::size_t node_count_ = 0;
  std::unordered_set<EdgeKey, EdgeKeyHash> edges_;
  EndpointQuotientStatistics stats_;
};

EndpointQuotientEngine::EndpointQuotientEngine(const Grammar &grammar,
                                               Relation &relation,
                                               std::size_t node_count)
    : impl_(new Impl(grammar, relation, node_count)) {}

EndpointQuotientEngine::~EndpointQuotientEngine() = default;

void EndpointQuotientEngine::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool EndpointQuotientEngine::addEdge(SymbolId symbol, NodeId source,
                                     NodeId target) {
  return impl_->addEdge(symbol, source, target);
}

EndpointQuotientStatistics EndpointQuotientEngine::solve() {
  return impl_->solve();
}

const EndpointQuotientStatistics &
EndpointQuotientEngine::statistics() const {
  return impl_->statistics();
}

} // namespace lotus::cfl::classical::engines