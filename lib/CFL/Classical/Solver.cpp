#include "CFL/Classical/Solver.h"

#include "CFL/Classical/HybridForest.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lotus::cfl::classical {
namespace {

bool isTransitiveRule(const BinaryRuleId &rule) {
  return rule.lhs == rule.first && rule.lhs == rule.second;
}

struct WorkItem {
  RelationEdge edge;
};

class HybridRelation final : public Relation {
public:
  HybridRelation(const std::unordered_set<SymbolId> &transitive_symbols,
                 std::size_t node_count)
      : base_(createRelation(RelationBackend::SparseBitVectors, node_count)) {
    for (SymbolId symbol : transitive_symbols) {
      forests_.emplace(symbol,
                       std::make_unique<HybridReachabilityForest>(node_count));
    }
  }

  void ensureNodeCount(std::size_t node_count) override {
    base_->ensureNodeCount(node_count);
    for (auto &[_, forest] : forests_) {
      forest->ensureNodeCount(node_count);
    }
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) override {
    if (auto it = forests_.find(symbol); it != forests_.end()) {
      const bool existed = it->second->hasPath(source, target);
      it->second->addArc(source, target);
      return !existed;
    }
    return base_->add(symbol, source, target);
  }

  std::vector<std::pair<NodeId, NodeId>>
  addTransitiveArc(SymbolId symbol, NodeId source, NodeId target) {
    const auto it = forests_.find(symbol);
    if (it == forests_.end()) {
      throw std::logic_error("Symbol has no hybrid reachability forest");
    }
    return it->second->addArc(source, target);
  }

  bool isTransitive(SymbolId symbol) const {
    return forests_.count(symbol) != 0;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const override {
    if (const auto it = forests_.find(symbol); it != forests_.end()) {
      return it->second->hasPath(source, target);
    }
    return base_->contains(symbol, source, target);
  }

  std::vector<NodeId> successors(SymbolId symbol,
                                 NodeId source) const override {
    if (const auto it = forests_.find(symbol); it != forests_.end()) {
      return it->second->successors(source);
    }
    return base_->successors(symbol, source);
  }

  std::vector<NodeId> predecessors(SymbolId symbol,
                                   NodeId target) const override {
    if (const auto it = forests_.find(symbol); it != forests_.end()) {
      return it->second->predecessors(target);
    }
    return base_->predecessors(symbol, target);
  }

  std::vector<RelationEdge> edges() const override {
    std::vector<RelationEdge> result = base_->edges();
    result.reserve(edgeCount());
    for (const auto &[symbol, forest] : forests_) {
      for (const auto &[source, target] : forest->edges()) {
        result.push_back({symbol, source, target});
      }
    }
    return result;
  }

  std::size_t edgeCount() const override {
    std::size_t count = base_->edgeCount();
    for (const auto &[_, forest] : forests_) {
      count += forest->edgeCount();
    }
    return count;
  }

  std::size_t approximateMemoryBytes() const override {
    std::size_t bytes = sizeof(*this) + base_->approximateMemoryBytes();
    for (const auto &[_, forest] : forests_) {
      bytes += forest->approximateMemoryBytes();
    }
    return bytes;
  }

  const auto &forests() const { return forests_; }

private:
  std::unique_ptr<Relation> base_;
  std::unordered_map<SymbolId, std::unique_ptr<HybridReachabilityForest>>
      forests_;
};

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
        relation_(backend == SolverBackend::Hybrid
                      ? std::unique_ptr<Relation>(new HybridRelation(
                            grammar.transitiveSymbols(), graph.vertexCount()))
                      : createRelation(backend == SolverBackend::Baseline
                                           ? RelationBackend::SparseSets
                                           : RelationBackend::SparseBitVectors,
                                       graph.vertexCount())) {
    for (const GrammarIssue &issue : grammar.validate()) {
      if (issue.severity == GrammarIssueSeverity::Error) {
        throw std::invalid_argument(issue.message);
      }
    }

    hybrid_relation_ = dynamic_cast<HybridRelation *>(relation_.get());

    for (const LabeledEdge &edge : graph.edges()) {
      if (!grammar.hasSymbol(edge.label)) {
        throw std::invalid_argument("Graph uses unknown grammar symbol: " +
                                    edge.label);
      }
      const SymbolId symbol = grammar.symbolId(edge.label);
      if (insertInputFact(symbol, edge.source, edge.target)) {
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
    const bool inserted = insertInputFact(symbol, source, target);
    input_edges_ += inserted ? 1 : 0;
    return inserted;
  }

  NodeId addNode(const std::string &name) {
    const NodeId node = graph_.addVertex(name);
    relation_->ensureNodeCount(graph_.vertexCount());
    return node;
  }

  ReachabilityStats solve() {
    const auto start = std::chrono::steady_clock::now();
    ReachabilityStats stats;
    stats.added_edges = pending_derived_edges_;
    pending_derived_edges_ = 0;
    stats.graph_nodes = graph_.vertexCount();
    stats.base_graph_edges = graph_.edges().size();
    stats.grammar_symbols = grammar_.symbolCount();
    stats.grammar_terminals = grammar_.terminals().size();
    stats.grammar_nonterminals = grammar_.nonterminals().size();
    stats.grammar_productions = grammar_.productionCount();
    stats.grammar_nullable_symbols = grammar_.nullableSymbols().size();
    stats.grammar_transitive_symbols = grammar_.transitiveSymbols().size();
    stats.input_edges = input_edges_;

    for (SymbolId symbol : grammar_.nullableSymbolIds()) {
      for (NodeId node = 0; node < graph_.vertexCount(); ++node) {
        addDerived(symbol, node, node, stats);
      }
    }

    while (!worklist_.empty()) {
      const WorkItem item = worklist_.back();
      worklist_.pop_back();
      ++stats.processed_work_items;
      const RelationEdge &selected = item.edge;

      if (const auto it = grammar_.unaryByRhsId().find(selected.symbol);
          it != grammar_.unaryByRhsId().end()) {
        for (SymbolId lhs : it->second) {
          ++stats.classical_iterations;
          addDerived(lhs, selected.source, selected.target, stats);
        }
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
    for (const RelationEdge &edge : relation_->edges()) {
      if (edge.symbol == grammar_.startSymbolId()) {
        ++stats.start_symbol_edges;
      }
    }
    stats.relation_memory_bytes = relation_->approximateMemoryBytes();
    stats.peak_worklist_size = peak_worklist_size_;
    collectHybridStatistics(stats);
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
                  ReachabilityStats &stats) {
    if (hybrid_relation_ && hybrid_relation_->isTransitive(symbol)) {
      const auto discovered =
          hybrid_relation_->addTransitiveArc(symbol, source, target);
      if (discovered.empty()) {
        ++stats.duplicate_edges;
        return;
      }
      stats.classical_iterations += discovered.size();
      for (const auto &[new_source, new_target] : discovered) {
        pushWorkItem({symbol, new_source, new_target});
        ++stats.added_edges;
      }
      return;
    }
    if (!relation_->add(symbol, source, target)) {
      ++stats.duplicate_edges;
      return;
    }
    pushWorkItem({symbol, source, target});
    ++stats.added_edges;
  }

  void collectHybridStatistics(ReachabilityStats &stats) const {
    if (!hybrid_relation_) {
      return;
    }
    for (const auto &[_, forest] : hybrid_relation_->forests()) {
      const HybridForestStatistics &forest_stats = forest->statistics();
      stats.hybrid_forest_roots += forest_stats.roots;
      stats.hybrid_forest_nodes += forest_stats.tree_nodes;
      stats.hybrid_forest_edges += forest_stats.tree_edges;
      stats.hybrid_arc_insertions += forest_stats.arc_insertions;
      stats.hybrid_meld_operations += forest_stats.meld_operations;
      stats.hybrid_duplicate_melds += forest_stats.duplicate_melds;
      stats.hybrid_forest_memory_bytes += forest->approximateMemoryBytes();
    }
  }

  bool insertInputFact(SymbolId symbol, NodeId source, NodeId target) {
    if (hybrid_relation_ && hybrid_relation_->isTransitive(symbol)) {
      const bool existed = relation_->contains(symbol, source, target);
      const auto discovered =
          hybrid_relation_->addTransitiveArc(symbol, source, target);
      for (const auto &[new_source, new_target] : discovered) {
        pushWorkItem({symbol, new_source, new_target});
      }
      if (!existed && !discovered.empty()) {
        pending_derived_edges_ += discovered.size() - 1;
      }
      return !existed;
    }
    if (!relation_->add(symbol, source, target)) {
      return false;
    }
    pushWorkItem({symbol, source, target});
    return true;
  }

  void pushWorkItem(const RelationEdge &edge) {
    worklist_.push_back({edge});
    peak_worklist_size_ = std::max(peak_worklist_size_, worklist_.size());
  }

  LabeledGraph &graph_;
  const Grammar &grammar_;
  SolverBackend backend_;
  std::unique_ptr<Relation> relation_;
  std::vector<WorkItem> worklist_;
  std::size_t input_edges_ = 0;
  std::size_t peak_worklist_size_ = 0;
  std::size_t pending_derived_edges_ = 0;
  HybridRelation *hybrid_relation_ = nullptr;
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
