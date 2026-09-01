#include "CFL/Classical/Solver.h"

#include <chrono>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {
namespace {

bool isTransitiveRule(const BinaryRuleId &rule) {
  return rule.lhs == rule.first && rule.lhs == rule.second;
}

} // namespace

const char *solverBackendName(SolverBackend backend) {
  switch (backend) {
  case SolverBackend::Baseline:
    return "baseline";
  case SolverBackend::POCR:
    return "pocr";
  case SolverBackend::Hybrid:
    return "hybrid";
  }
  return "unknown";
}

class SolverSession::Impl {
public:
  Impl(LabeledGraph &graph, const Grammar &grammar, SolverBackend backend)
      : graph_(graph), grammar_(grammar), backend_(backend),
        relation_(createRelation(backend == SolverBackend::Baseline
                                     ? RelationBackend::SparseSets
                                     : RelationBackend::SparseBitVectors,
                                 graph.vertexCount())) {
    for (const GrammarIssue &issue : grammar.validate()) {
      if (issue.severity == GrammarIssueSeverity::Error) {
        throw std::invalid_argument(issue.message);
      }
    }

    for (const LabeledEdge &edge : graph.edges()) {
      if (!grammar.hasSymbol(edge.label)) {
        throw std::invalid_argument("Graph uses unknown grammar symbol: " +
                                    edge.label);
      }
      const SymbolId symbol = grammar.symbolId(edge.label);
      if (relation_->add(symbol, edge.source, edge.target)) {
        worklist_.push_back({symbol, edge.source, edge.target});
        ++input_edges_;
      }
    }
  }

  bool addTerminalEdge(NodeId source, NodeId target, const std::string &label) {
    if (!grammar_.isTerminal(label)) {
      throw std::invalid_argument("Incremental edge is not a terminal: " +
                                  label);
    }
    graph_.addEdge(source, target, label);
    const SymbolId symbol = grammar_.symbolId(label);
    if (!relation_->add(symbol, source, target)) {
      return false;
    }
    worklist_.push_back({symbol, source, target});
    ++input_edges_;
    return true;
  }

  NodeId addNode(const std::string &name) {
    const NodeId node = graph_.addVertex(name);
    relation_->ensureNodeCount(graph_.vertexCount());
    return node;
  }

  ReachabilityStats solve() {
    const auto start = std::chrono::steady_clock::now();
    ReachabilityStats stats;
    stats.input_edges = input_edges_;

    for (SymbolId symbol : grammar_.nullableSymbolIds()) {
      for (NodeId node = 0; node < graph_.vertexCount(); ++node) {
        addDerived(symbol, node, node, stats);
      }
    }

    while (!worklist_.empty()) {
      const RelationEdge selected = worklist_.back();
      worklist_.pop_back();

      if (const auto it = grammar_.unaryByRhsId().find(selected.symbol);
          it != grammar_.unaryByRhsId().end()) {
        for (SymbolId lhs : it->second) {
          ++stats.classical_iterations;
          addDerived(lhs, selected.source, selected.target, stats);
        }
      }

      if (backend_ == SolverBackend::Hybrid &&
          grammar_.transitiveSymbols().count(selected.symbol) != 0 &&
          hybrid_closed_edges_.count(
              {selected.symbol, selected.source, selected.target}) == 0) {
        processTransitive(selected, stats);
      }

      if (const auto it = grammar_.binaryByFirstId().find(selected.symbol);
          it != grammar_.binaryByFirstId().end()) {
        for (const BinaryRuleId &rule : it->second) {
          if (backend_ == SolverBackend::Hybrid && isTransitiveRule(rule)) {
            continue;
          }
          for (NodeId target :
               relation_->successors(rule.second, selected.target)) {
            ++stats.classical_iterations;
            addDerived(rule.lhs, selected.source, target, stats);
          }
        }
      }

      if (const auto it = grammar_.binaryBySecondId().find(selected.symbol);
          it != grammar_.binaryBySecondId().end()) {
        for (const BinaryRuleId &rule : it->second) {
          if (backend_ == SolverBackend::Hybrid && isTransitiveRule(rule)) {
            continue;
          }
          for (NodeId source :
               relation_->predecessors(rule.first, selected.source)) {
            ++stats.classical_iterations;
            addDerived(rule.lhs, source, selected.target, stats);
          }
        }
      }
    }

    stats.relation_edges = relation_->edgeCount();
    stats.solve_time_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    return stats;
  }

  bool contains(NodeId source, NodeId target, const std::string &label) const {
    return grammar_.hasSymbol(label) &&
           relation_->contains(grammar_.symbolId(label), source, target);
  }

  const Relation &relation() const { return *relation_; }

private:
  void addDerived(SymbolId symbol, NodeId source, NodeId target,
                  ReachabilityStats &stats, bool hybrid_closed = false) {
    if (!relation_->add(symbol, source, target)) {
      return;
    }
    if (hybrid_closed) {
      hybrid_closed_edges_.insert({symbol, source, target});
    }
    worklist_.push_back({symbol, source, target});
    ++stats.added_edges;
  }

  void processTransitive(const RelationEdge &selected,
                         ReachabilityStats &stats) {
    auto sources = relation_->predecessors(selected.symbol, selected.source);
    auto targets = relation_->successors(selected.symbol, selected.target);
    sources.push_back(selected.source);
    targets.push_back(selected.target);

    for (NodeId source : sources) {
      for (NodeId target : targets) {
        ++stats.classical_iterations;
        addDerived(selected.symbol, source, target, stats, true);
      }
    }
  }

  LabeledGraph &graph_;
  const Grammar &grammar_;
  SolverBackend backend_;
  std::unique_ptr<Relation> relation_;
  std::vector<RelationEdge> worklist_;
  std::size_t input_edges_ = 0;
  std::set<std::tuple<SymbolId, NodeId, NodeId>> hybrid_closed_edges_;
};

SolverSession::SolverSession(LabeledGraph &graph, const Grammar &grammar,
                             SolverBackend backend)
    : impl_(std::make_unique<Impl>(graph, grammar, backend)) {}

SolverSession::~SolverSession() = default;
SolverSession::SolverSession(SolverSession &&) noexcept = default;
SolverSession &SolverSession::operator=(SolverSession &&) noexcept = default;

std::size_t SolverSession::addNode(const std::string &name) {
  return impl_->addNode(name);
}

bool SolverSession::addTerminalEdge(std::size_t source, std::size_t target,
                                    const std::string &label) {
  return impl_->addTerminalEdge(source, target, label);
}

ReachabilityStats SolverSession::solve() { return impl_->solve(); }

bool SolverSession::contains(std::size_t source, std::size_t target,
                             const std::string &label) const {
  return impl_->contains(source, target, label);
}

const Relation &SolverSession::relation() const { return impl_->relation(); }

} // namespace lotus::cfl::classical
