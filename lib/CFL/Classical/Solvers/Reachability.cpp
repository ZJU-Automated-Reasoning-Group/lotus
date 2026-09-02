#include "CFL/Classical/Solvers/Reachability.h"

#include "CFL/Classical/Solvers/TransitiveClosure.h"

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

class TransitiveRelation final : public Relation {
public:
  TransitiveRelation(const std::unordered_set<SymbolId> &transitive_symbols,
                     std::size_t node_count)
      : base_(createRelation(RelationBackend::SparseBitVectors, node_count)) {
    for (SymbolId symbol : transitive_symbols) {
      closures_.emplace(
          symbol, std::make_unique<IncrementalTransitiveClosure>(node_count));
    }
  }

  void ensureNodeCount(std::size_t node_count) override {
    base_->ensureNodeCount(node_count);
    for (auto &[_, closure] : closures_) {
      closure->ensureNodeCount(node_count);
    }
  }

  bool add(SymbolId symbol, NodeId source, NodeId target) override {
    if (auto it = closures_.find(symbol); it != closures_.end()) {
      const bool existed = it->second->hasPath(source, target);
      it->second->addArc(source, target);
      return !existed;
    }
    return base_->add(symbol, source, target);
  }

  std::vector<std::pair<NodeId, NodeId>>
  addTransitiveArc(SymbolId symbol, NodeId source, NodeId target) {
    const auto it = closures_.find(symbol);
    if (it == closures_.end()) {
      throw std::logic_error("Symbol has no incremental transitive closure");
    }
    return it->second->addArc(source, target);
  }

  bool isTransitive(SymbolId symbol) const {
    return closures_.count(symbol) != 0;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const override {
    if (const auto it = closures_.find(symbol); it != closures_.end()) {
      return it->second->hasPath(source, target);
    }
    return base_->contains(symbol, source, target);
  }

  void
  forEachSuccessor(SymbolId symbol, NodeId source,
                   llvm::function_ref<void(NodeId)> visitor) const override {
    if (const auto it = closures_.find(symbol); it != closures_.end()) {
      it->second->forEachSuccessor(source, visitor);
      return;
    }
    base_->forEachSuccessor(symbol, source, visitor);
  }

  void
  forEachPredecessor(SymbolId symbol, NodeId target,
                     llvm::function_ref<void(NodeId)> visitor) const override {
    if (const auto it = closures_.find(symbol); it != closures_.end()) {
      it->second->forEachPredecessor(target, visitor);
      return;
    }
    base_->forEachPredecessor(symbol, target, visitor);
  }

  std::vector<RelationEdge> edges() const override {
    std::vector<RelationEdge> result = base_->edges();
    result.reserve(edgeCount());
    for (const auto &[symbol, closure] : closures_) {
      for (const auto &[source, target] : closure->edges()) {
        result.push_back({symbol, source, target});
      }
    }
    return result;
  }

  std::vector<RelationEdge> edges(SymbolId symbol) const override {
    if (const auto it = closures_.find(symbol); it != closures_.end()) {
      std::vector<RelationEdge> result;
      result.reserve(it->second->edgeCount());
      for (const auto &[source, target] : it->second->edges()) {
        result.push_back({symbol, source, target});
      }
      return result;
    }
    return base_->edges(symbol);
  }

  std::size_t edgeCount() const override {
    std::size_t count = base_->edgeCount();
    for (const auto &[_, closure] : closures_) {
      count += closure->edgeCount();
    }
    return count;
  }

  std::size_t edgeCount(SymbolId symbol) const override {
    if (const auto it = closures_.find(symbol); it != closures_.end()) {
      return it->second->edgeCount();
    }
    return base_->edgeCount(symbol);
  }

  std::size_t approximateMemoryBytes() const override {
    std::size_t bytes = sizeof(*this) + base_->approximateMemoryBytes();
    for (const auto &[_, closure] : closures_) {
      bytes += closure->approximateMemoryBytes();
    }
    return bytes;
  }

  const auto &closures() const { return closures_; }

private:
  std::unique_ptr<Relation> base_;
  std::unordered_map<SymbolId, std::unique_ptr<IncrementalTransitiveClosure>>
      closures_;
};

} // namespace

const char *solverBackendName(SolverBackend backend) {
  switch (backend) {
  case SolverBackend::SparseSet:
    return "sparse-set";
  case SolverBackend::SparseBitVector:
    return "sparse-bitvector";
  case SolverBackend::TransitiveClosure:
    return "transitive-closure";
  }
  return "unknown";
}

class SolverSession::Impl {
public:
  Impl(LabeledGraph &graph, const Grammar &grammar, SolverBackend backend)
      : graph_(graph), grammar_(grammar), backend_(backend),
        relation_(backend == SolverBackend::TransitiveClosure
                      ? std::unique_ptr<Relation>(new TransitiveRelation(
                            grammar.transitiveSymbols(), graph.vertexCount()))
                      : createRelation(backend == SolverBackend::SparseSet
                                           ? RelationBackend::SparseSets
                                           : RelationBackend::SparseBitVectors,
                                       graph.vertexCount())),
        expected_graph_version_(graph.mutationVersion()) {
    for (const GrammarIssue &issue : grammar.validate()) {
      if (issue.severity == GrammarIssueSeverity::Error) {
        throw std::invalid_argument(issue.message);
      }
    }

    transitive_relation_ = dynamic_cast<TransitiveRelation *>(relation_.get());

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
    validateGraphVersion();
    if (!grammar_.isTerminal(label)) {
      throw std::invalid_argument("Incremental edge is not a terminal: " +
                                  label);
    }
    graph_.addEdge(source, target, label);
    expected_graph_version_ = graph_.mutationVersion();
    const SymbolId symbol = grammar_.symbolId(label);
    const bool inserted = insertInputFact(symbol, source, target);
    input_edges_ += inserted ? 1 : 0;
    return inserted;
  }

  NodeId addNode(const std::string &name) {
    validateGraphVersion();
    const NodeId node = graph_.addVertex(name);
    expected_graph_version_ = graph_.mutationVersion();
    relation_->ensureNodeCount(graph_.vertexCount());
    return node;
  }

  ReachabilityStats solve() {
    validateGraphVersion();
    const auto start = std::chrono::steady_clock::now();
    ReachabilityStats stats;
    stats.added_edges = pending_derived_edges_;
    pending_derived_edges_ = 0;
    stats.graph_nodes = graph_.vertexCount();
    stats.base_graph_edges = graph_.edgeCount();
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
          if (backend_ == SolverBackend::TransitiveClosure &&
              isTransitiveRule(rule)) {
            continue;
          }
          join_candidates_.clear();
          relation_->forEachSuccessor(
              rule.second, selected.target,
              [&](NodeId target) { join_candidates_.push_back(target); });
          for (NodeId target : join_candidates_) {
            ++stats.classical_iterations;
            addDerived(rule.lhs, selected.source, target, stats);
          }
        }
      }

      if (const auto it = grammar_.binaryBySecondId().find(selected.symbol);
          it != grammar_.binaryBySecondId().end()) {
        for (const BinaryRuleId &rule : it->second) {
          if (backend_ == SolverBackend::TransitiveClosure &&
              isTransitiveRule(rule)) {
            continue;
          }
          join_candidates_.clear();
          relation_->forEachPredecessor(
              rule.first, selected.source,
              [&](NodeId source) { join_candidates_.push_back(source); });
          for (NodeId source : join_candidates_) {
            ++stats.classical_iterations;
            addDerived(rule.lhs, source, selected.target, stats);
          }
        }
      }
    }

    stats.relation_edges = relation_->edgeCount();
    stats.start_symbol_edges = relation_->edgeCount(grammar_.startSymbolId());
    stats.relation_memory_bytes = relation_->approximateMemoryBytes();
    stats.peak_worklist_size = peak_worklist_size_;
    collectTransitiveStatistics(stats);
    stats.solve_time_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    return stats;
  }

  bool contains(NodeId source, NodeId target, const std::string &label) const {
    validateGraphVersion();
    return grammar_.hasSymbol(label) &&
           relation_->contains(grammar_.symbolId(label), source, target);
  }

  const Relation &relation() const {
    validateGraphVersion();
    return *relation_;
  }

  bool addKnownRelationEdge(NodeId source, NodeId target,
                            const std::string &label) {
    validateGraphVersion();
    if (!grammar_.hasSymbol(label)) {
      throw std::invalid_argument("Unknown migrated relation symbol: " + label);
    }
    return insertInputFact(grammar_.symbolId(label), source, target);
  }

private:
  void validateGraphVersion() const {
    if (graph_.mutationVersion() != expected_graph_version_) {
      throw std::logic_error(
          "LabeledGraph was mutated outside its SolverSession; use "
          "SolverSession::addNode or addTerminalEdge");
    }
  }

  void addDerived(SymbolId symbol, NodeId source, NodeId target,
                  ReachabilityStats &stats) {
    if (transitive_relation_ && transitive_relation_->isTransitive(symbol)) {
      const auto discovered =
          transitive_relation_->addTransitiveArc(symbol, source, target);
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

  void collectTransitiveStatistics(ReachabilityStats &stats) const {
    if (!transitive_relation_) {
      return;
    }
    for (const auto &[_, closure] : transitive_relation_->closures()) {
      const TransitiveClosureStatistics &closure_stats = closure->statistics();
      ++stats.transitive_closure_instances;
      stats.transitive_relation_edges += closure_stats.relation_edges;
      stats.transitive_arc_insertions += closure_stats.arc_insertions;
      stats.transitive_propagated_pairs += closure_stats.propagated_pairs;
      stats.transitive_duplicate_pairs += closure_stats.duplicate_pairs;
      stats.transitive_memory_bytes += closure->approximateMemoryBytes();
    }
  }

  bool insertInputFact(SymbolId symbol, NodeId source, NodeId target) {
    if (transitive_relation_ && transitive_relation_->isTransitive(symbol)) {
      const bool existed = relation_->contains(symbol, source, target);
      const auto discovered =
          transitive_relation_->addTransitiveArc(symbol, source, target);
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
  std::vector<NodeId> join_candidates_;
  std::size_t input_edges_ = 0;
  std::size_t peak_worklist_size_ = 0;
  std::size_t pending_derived_edges_ = 0;
  TransitiveRelation *transitive_relation_ = nullptr;
  std::uint64_t expected_graph_version_ = 0;
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

bool SolverSession::addKnownRelationEdge(std::size_t source, std::size_t target,
                                         const std::string &label) {
  return impl_->addKnownRelationEdge(source, target, label);
}

ReachabilityStats SolverSession::solve() { return impl_->solve(); }

bool SolverSession::contains(std::size_t source, std::size_t target,
                             const std::string &label) const {
  return impl_->contains(source, target, label);
}

const Relation &SolverSession::relation() const { return impl_->relation(); }

} // namespace lotus::cfl::classical
