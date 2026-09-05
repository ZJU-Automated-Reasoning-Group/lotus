#include "CFL/Classical/Solvers/SolverSession.h"

#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotientEngine.h"
#include "CFL/Classical/Solvers/Engines/PEARL/PearlEngine.h"
#include "CFL/Classical/Solvers/Engines/POCR/FullyOrderedClosure.h"
#include "CFL/Classical/Solvers/Engines/POCR/PairedTreeClosure.h"
#include "CFL/Classical/Solvers/Engines/SQID/SqidEngine.h"
#include "CFL/Classical/Solvers/Engines/TransitiveClosure.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical {
namespace {

bool isTransitiveRule(const BinaryRuleId &rule) {
  return rule.lhs == rule.first && rule.lhs == rule.second;
}

struct WorkItem {
  RelationEdge edge;
  bool primary = false;
};

/// Forward-only relation used by POCR's per-source Graspan epochs.
class GraspanData {
public:
  GraspanData(std::size_t symbol_count, std::size_t node_count)
      : symbol_count_(symbol_count), node_count_(node_count) {}

  void ensureNodeCount(std::size_t node_count) { node_count_ = node_count; }

  bool add(SymbolId symbol, NodeId source, NodeId target) {
    require(symbol, source, target);
    if (!successors_[source][symbol].test_and_set(
            static_cast<unsigned>(target))) {
      return false;
    }
    ++edge_count_;
    return true;
  }

  bool contains(SymbolId symbol, NodeId source, NodeId target) const {
    require(symbol, source, target);
    return successors(symbol, source).test(static_cast<unsigned>(target));
  }

  const llvm::SparseBitVector<> &successors(SymbolId symbol,
                                            NodeId source) const {
    require(symbol, source, source);
    const auto source_it = successors_.find(source);
    if (source_it == successors_.end()) {
      return empty_;
    }
    const auto symbol_it = source_it->second.find(symbol);
    return symbol_it == source_it->second.end() ? empty_ : symbol_it->second;
  }

  std::vector<llvm::SparseBitVector<>> copySource(NodeId source) const {
    if (source >= node_count_) {
      throw std::out_of_range("Graspan source is out of range");
    }
    std::vector<llvm::SparseBitVector<>> result(symbol_count_);
    const auto source_it = successors_.find(source);
    if (source_it == successors_.end()) {
      return result;
    }
    for (const auto &[symbol, targets] : source_it->second) {
      result.at(symbol) = targets;
    }
    return result;
  }

  void clearSource(NodeId source) {
    const auto source_it = successors_.find(source);
    if (source_it == successors_.end()) {
      return;
    }
    for (const auto &[_, targets] : source_it->second) {
      edge_count_ -= targets.count();
    }
    successors_.erase(source_it);
  }

  std::size_t symbolCount() const { return symbol_count_; }
  std::size_t edgeCount() const { return edge_count_; }

private:
  void require(SymbolId symbol, NodeId source, NodeId target) const {
    if (symbol >= symbol_count_ || source >= node_count_ ||
        target >= node_count_) {
      throw std::out_of_range("Graspan relation endpoint is out of range");
    }
  }

  std::unordered_map<NodeId, std::map<SymbolId, llvm::SparseBitVector<>>>
      successors_;
  llvm::SparseBitVector<> empty_;
  std::size_t symbol_count_ = 0;
  std::size_t node_count_ = 0;
  std::size_t edge_count_ = 0;
};

template <typename Closure> class ClosureRelation final : public Relation {
public:
  template <typename... Arguments>
  ClosureRelation(const std::unordered_set<SymbolId> &transitive_symbols,
                  std::size_t node_count, Arguments &&...arguments)
      : base_(createRelation(RelationBackend::SparseBitVectors, node_count)) {
    for (SymbolId symbol : transitive_symbols) {
      closures_.emplace(symbol,
                        std::make_unique<Closure>(
                            node_count, std::forward<Arguments>(arguments)...));
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
      throw std::logic_error("Symbol has no specialized transitive closure");
    }
    return it->second->addArc(source, target);
  }

  bool addPrimaryArc(SymbolId symbol, NodeId source, NodeId target) {
    const auto it = closures_.find(symbol);
    if (it == closures_.end()) {
      throw std::logic_error("Symbol has no specialized transitive closure");
    }
    return it->second->addPrimaryArc(source, target);
  }

  std::vector<std::pair<NodeId, NodeId>>
  closePrimaryArc(SymbolId symbol, NodeId source, NodeId target) {
    const auto it = closures_.find(symbol);
    if (it == closures_.end()) {
      throw std::logic_error("Symbol has no specialized transitive closure");
    }
    return it->second->closePrimaryArc(source, target);
  }

  const Closure &closure(SymbolId symbol) const {
    const auto it = closures_.find(symbol);
    if (it == closures_.end()) {
      throw std::logic_error("Symbol has no specialized transitive closure");
    }
    return *it->second;
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

  std::size_t estimatedPayloadBytes() const override {
    std::size_t bytes = sizeof(*this) + base_->estimatedPayloadBytes();
    for (const auto &[_, closure] : closures_) {
      bytes += closure->estimatedPayloadBytes();
    }
    return bytes;
  }

  const auto &closures() const { return closures_; }

private:
  std::unique_ptr<Relation> base_;
  std::unordered_map<SymbolId, std::unique_ptr<Closure>> closures_;
};

using BitVectorClosureRelation =
    ClosureRelation<engines::IncrementalTransitiveClosure>;
using PocrClosureRelation = ClosureRelation<engines::PocrTransitiveClosure>;
using FullyOrderedClosureRelation =
    ClosureRelation<engines::FullyOrderedTransitiveClosure>;

std::unique_ptr<Relation>
createSolverRelation(SolverBackend backend,
                     const std::unordered_set<SymbolId> &transitive_symbols,
                     std::size_t node_count, bool simplify_focr_cycles) {
  switch (backend) {
  case SolverBackend::SparseSet:
    return createRelation(RelationBackend::SparseSets, node_count);
  case SolverBackend::SparseBitVector:
    return createRelation(RelationBackend::SparseBitVectors, node_count);
  case SolverBackend::Graspan:
  case SolverBackend::Sqid:
  case SolverBackend::Pearl:
    return createRelation(RelationBackend::SparseBitVectors, node_count);
  case SolverBackend::TransitiveClosure:
    return std::make_unique<BitVectorClosureRelation>(transitive_symbols,
                                                      node_count);
  case SolverBackend::Pocr:
  case SolverBackend::HierarchicalPocr:
    return std::make_unique<PocrClosureRelation>(transitive_symbols,
                                                 node_count);
  case SolverBackend::FullyOrdered:
    return std::make_unique<FullyOrderedClosureRelation>(
        transitive_symbols, node_count, simplify_focr_cycles);
  case SolverBackend::EndpointQuotient:
    return createRelation(RelationBackend::SparseSets, node_count);
  }
  throw std::invalid_argument("Unknown CFL solver backend");
}

} // namespace

const char *solverBackendName(SolverBackend backend) {
  switch (backend) {
  case SolverBackend::SparseSet:
    return "sparse-set";
  case SolverBackend::SparseBitVector:
    return "sparse-bitvector";
  case SolverBackend::Graspan:
    return "graspan";
  case SolverBackend::Sqid:
    return "sqid";
  case SolverBackend::Pearl:
    return "pearl";
  case SolverBackend::TransitiveClosure:
    return "transitive-closure";
  case SolverBackend::Pocr:
    return "pocr";
  case SolverBackend::HierarchicalPocr:
    return "hpocr";
  case SolverBackend::FullyOrdered:
    return "focr";
  case SolverBackend::EndpointQuotient:
    return "endpoint-quotient";
  }
  return "unknown";
}

SolverBackend parseSolverBackend(std::string_view name) {
  if (name == "sparse-set") {
    return SolverBackend::SparseSet;
  }
  if (name == "sparse-bitvector") {
    return SolverBackend::SparseBitVector;
  }
  if (name == "graspan") {
    return SolverBackend::Graspan;
  }
  if (name == "sqid") {
    return SolverBackend::Sqid;
  }
  if (name == "pearl") {
    return SolverBackend::Pearl;
  }
  if (name == "transitive-closure") {
    return SolverBackend::TransitiveClosure;
  }
  if (name == "pocr") {
    return SolverBackend::Pocr;
  }
  if (name == "hpocr") {
    return SolverBackend::HierarchicalPocr;
  }
  if (name == "focr") {
    return SolverBackend::FullyOrdered;
  }
  if (name == "endpoint-quotient") {
    return SolverBackend::EndpointQuotient;
  }
  throw std::invalid_argument("Unknown solver: " + std::string(name));
}

class SolverSession::Impl {
public:
  Impl(LabeledGraph &graph, const Grammar &grammar,
       const SolverOptions &options)
      : graph_(graph), grammar_(grammar), backend_(options.backend),
        unidirectional_(options.unidirectional),
        relation_(createSolverRelation(
            options.backend, grammar.transitiveSymbols(), graph.vertexCount(),
            options.simplify_focr_cycles)),
        expected_graph_version_(graph.mutationVersion()) {
    for (const GrammarIssue &issue : grammar.validate()) {
      if (issue.severity == GrammarIssueSeverity::Error) {
        throw std::invalid_argument(issue.message);
      }
    }

    transitive_relation_ =
        dynamic_cast<BitVectorClosureRelation *>(relation_.get());
    pocr_relation_ = dynamic_cast<PocrClosureRelation *>(relation_.get());
    fully_ordered_relation_ =
        dynamic_cast<FullyOrderedClosureRelation *>(relation_.get());
    if (backend_ == SolverBackend::Graspan) {
      graspan_old_ = std::make_unique<GraspanData>(grammar.symbolCount(),
                                                   graph.vertexCount());
      graspan_current_ = std::make_unique<GraspanData>(grammar.symbolCount(),
                                                       graph.vertexCount());
    }
    if (backend_ == SolverBackend::Sqid) {
      sqid_engine_ = std::make_unique<engines::SqidEngine>(grammar, *relation_,
                                                           graph.vertexCount());
    }
    if (backend_ == SolverBackend::Pearl) {
      engines::PearlOptions pearl_options;
      for (const auto &[first, second] : options.pearl_inverse_relations) {
        if (!grammar.hasSymbol(first) || !grammar.hasSymbol(second)) {
          throw std::invalid_argument(
              "PEARL inverse relation uses an unknown grammar symbol");
        }
        pearl_options.inverse_relations.push_back(
            {grammar.symbolId(first), grammar.symbolId(second)});
      }
      pearl_engine_ = std::make_unique<engines::PearlEngine>(
          grammar, *relation_, graph.vertexCount(), std::move(pearl_options));
    }
    if (backend_ == SolverBackend::EndpointQuotient) {
      eq_engine_ = std::make_unique<engines::EndpointQuotientEngine>(
          grammar, *relation_, graph.vertexCount());
    }
    if (unidirectional_) {
      candidate_relation_ = createRelation(RelationBackend::SparseBitVectors,
                                           graph.vertexCount());
    }

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
    if (graspan_old_) {
      graspan_old_->ensureNodeCount(graph_.vertexCount());
      graspan_current_->ensureNodeCount(graph_.vertexCount());
    }
    if (sqid_engine_) {
      sqid_engine_->ensureNodeCount(graph_.vertexCount());
    }
    if (pearl_engine_) {
      pearl_engine_->ensureNodeCount(graph_.vertexCount());
    }
    if (eq_engine_) {
      eq_engine_->ensureNodeCount(graph_.vertexCount());
    }
    if (candidate_relation_) {
      candidate_relation_->ensureNodeCount(graph_.vertexCount());
    }
    return node;
  }

  ReachabilityStats solve() {
    validateGraphVersion();
    const auto start = std::chrono::steady_clock::now();
    const TransitiveCounters transitive_before = transitiveCounters();
    const PocrCounters pocr_before = pocrCounters();
    const FullyOrderedCounters fully_ordered_before = fullyOrderedCounters();
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

    if (backend_ != SolverBackend::Pearl && backend_ != SolverBackend::Sqid &&
        backend_ != SolverBackend::EndpointQuotient) {
      for (SymbolId symbol : grammar_.nullableSymbolIds()) {
        for (NodeId node = nullable_seeded_nodes_; node < graph_.vertexCount();
             ++node) {
          addDerived(symbol, node, node, stats, true);
        }
      }
      nullable_seeded_nodes_ = graph_.vertexCount();
    }

    if (backend_ == SolverBackend::Pearl) {
      const engines::PearlStatistics pearl = pearl_engine_->solve();
      stats.classical_iterations += pearl.batch_propagations;
      stats.processed_work_items += pearl.non_transitive_items +
                                    pearl.partially_transitive_nodes +
                                    pearl.fully_transitive_primary_edges;
      stats.duplicate_edges += pearl.duplicate_edges;
      stats.added_edges += pearl.derived_edges;
    } else if (backend_ == SolverBackend::Sqid) {
      const engines::SqidStatistics sqid = sqid_engine_->solve();
      stats.classical_iterations += sqid.chaining_products;
      stats.processed_work_items +=
          sqid.processed_in_keys + sqid.processed_out_keys;
      stats.duplicate_edges += sqid.duplicate_edges;
      stats.added_edges += sqid.derived_edges;
      stats.peak_worklist_size =
          std::max(sqid.peak_in_worklist, sqid.peak_out_worklist);
    } else if (backend_ == SolverBackend::Graspan) {
      solveGraspan(stats);
    } else if (backend_ == SolverBackend::EndpointQuotient) {
      const engines::EndpointQuotientStatistics eq = eq_engine_->solve();
      stats.classical_iterations += eq.binary_joins;
      stats.processed_work_items += eq.worklist_pops;
      stats.duplicate_edges += eq.duplicate_facts;
      stats.added_edges += eq.derived_facts;
      stats.peak_worklist_size =
          std::max(stats.peak_worklist_size, eq.peak_worklist);
      stats.endpoint_quotient_cells = eq.cells;
      stats.endpoint_quotient_facts = eq.logical_facts;
      stats.endpoint_quotient_seed_facts = eq.seed_facts;
      stats.endpoint_quotient_inferred_facts = eq.inferred_facts;
      stats.endpoint_quotient_binary_joins = eq.binary_joins;
      stats.endpoint_quotient_bridge_pairs = eq.bridge_pairs;
      stats.endpoint_quotient_source_classes = eq.source_classes;
      stats.endpoint_quotient_target_classes = eq.target_classes;
      stats.endpoint_quotient_nullable_symbols = eq.nullable_symbols;
      stats.endpoint_quotient_preprocess_us = eq.preprocess_us;
      stats.endpoint_quotient_saturation_us = eq.saturation_us;
      stats.endpoint_quotient_count_us = eq.count_us;
    } else if (backend_ == SolverBackend::HierarchicalPocr) {
      do {
        while (!primary_worklist_.empty()) {
          const WorkItem item = primary_worklist_.front();
          primary_worklist_.pop_front();
          processWorkItem(item, stats);
        }
        while (!worklist_.empty()) {
          const WorkItem item = worklist_.front();
          worklist_.pop_front();
          processWorkItem(item, stats);
        }
      } while (!primary_worklist_.empty());
    } else {
      while (!worklist_.empty()) {
        const WorkItem item = worklist_.front();
        worklist_.pop_front();
        processWorkItem(item, stats);
      }
    }

    stats.relation_edges = relation_->edgeCount();
    stats.start_symbol_edges = relation_->edgeCount(grammar_.startSymbolId());
    if (!grammar_.countSymbols().empty()) {
      std::set<std::pair<NodeId, NodeId>> counted_pairs;
      for (const std::string &symbol : grammar_.countSymbols()) {
        for (const RelationEdge &edge :
             relation_->edges(grammar_.symbolId(symbol))) {
          if (edge.source != edge.target) {
            counted_pairs.insert({edge.source, edge.target});
          }
        }
      }
      stats.count_symbol_edges = counted_pairs.size();
    }
    stats.relation_payload_bytes_estimate = relation_->estimatedPayloadBytes();
    stats.candidate_relation_edges = candidate_relation_
                                         ? candidate_relation_->edgeCount()
                                         : relation_->edgeCount();
    stats.peak_worklist_size =
        std::max(stats.peak_worklist_size, current_peak_worklist_size_);
    current_peak_worklist_size_ = primary_worklist_.size() + worklist_.size();
    collectTransitiveStatistics(stats, transitive_before);
    collectPocrStatistics(stats, pocr_before);
    collectFullyOrderedStatistics(stats, fully_ordered_before);
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
  struct TransitiveCounters {
    std::size_t arc_insertions = 0;
    std::size_t propagated_pairs = 0;
    std::size_t duplicate_pairs = 0;
  };

  struct PocrCounters {
    std::size_t traversal_steps = 0;
  };

  struct FullyOrderedCounters {
    std::size_t reachability_checks = 0;
    std::size_t critical_edge_insertions = 0;
    std::size_t critical_edge_removals = 0;
    std::size_t cycle_simplifications = 0;
  };

  void validateGraphVersion() const {
    if (graph_.mutationVersion() != expected_graph_version_) {
      throw std::logic_error(
          "LabeledGraph was mutated outside its SolverSession; use "
          "SolverSession::addNode or addTerminalEdge");
    }
  }

  void addGraspanResult(std::vector<llvm::SparseBitVector<>> &result,
                        SymbolId symbol, NodeId target,
                        ReachabilityStats &stats) {
    ++stats.classical_iterations;
    if (!result.at(symbol).test_and_set(static_cast<unsigned>(target))) {
      ++stats.duplicate_edges;
    }
  }

  void solveGraspanSource(NodeId source, ReachabilityStats &stats) {
    const auto current = graspan_current_->copySource(source);
    std::size_t current_edge_count = 0;
    for (const auto &targets : current) {
      current_edge_count += targets.count();
    }
    stats.processed_work_items += current_edge_count;

    std::vector<llvm::SparseBitVector<>> result(grammar_.symbolCount());

    // old + new. This is POCR's first Graspan phase for a source.
    for (SymbolId left = 0; left < grammar_.symbolCount(); ++left) {
      const auto rules = grammar_.binaryByFirstId().find(left);
      if (rules == grammar_.binaryByFirstId().end()) {
        continue;
      }
      for (unsigned middle : graspan_old_->successors(left, source)) {
        for (const BinaryRuleId &rule : rules->second) {
          for (unsigned target :
               graspan_current_->successors(rule.second, middle)) {
            addGraspanResult(result, rule.lhs, target, stats);
          }
        }
      }
    }

    // new unary, new + old, and new + new. The current relation is mutated
    // after each source, matching POCR's source-ordered epoch evaluation.
    for (SymbolId left = 0; left < grammar_.symbolCount(); ++left) {
      if (const auto unary = grammar_.unaryByRhsId().find(left);
          unary != grammar_.unaryByRhsId().end()) {
        for (SymbolId lhs : unary->second) {
          for (unsigned target : current[left]) {
            addGraspanResult(result, lhs, target, stats);
          }
        }
      }

      const auto rules = grammar_.binaryByFirstId().find(left);
      if (rules == grammar_.binaryByFirstId().end()) {
        continue;
      }
      for (unsigned middle : current[left]) {
        for (const BinaryRuleId &rule : rules->second) {
          for (unsigned target :
               graspan_old_->successors(rule.second, middle)) {
            addGraspanResult(result, rule.lhs, target, stats);
          }
          for (unsigned target :
               graspan_current_->successors(rule.second, middle)) {
            addGraspanResult(result, rule.lhs, target, stats);
          }
        }
      }
    }

    for (SymbolId symbol = 0; symbol < grammar_.symbolCount(); ++symbol) {
      for (unsigned target : current[symbol]) {
        graspan_old_->add(symbol, source, target);
      }
    }
    graspan_current_->clearSource(source);

    for (SymbolId symbol = 0; symbol < grammar_.symbolCount(); ++symbol) {
      for (unsigned target : result[symbol]) {
        if (graspan_old_->contains(symbol, source, target)) {
          ++stats.duplicate_edges;
          continue;
        }
        graspan_current_->add(symbol, source, target);
        if (relation_->add(symbol, source, target)) {
          addCandidate(symbol, source, target, false);
          ++stats.added_edges;
        }
      }
    }
    current_peak_worklist_size_ =
        std::max(current_peak_worklist_size_, graspan_current_->edgeCount());
  }

  void solveGraspan(ReachabilityStats &stats) {
    while (graspan_current_->edgeCount() != 0) {
      ++stats.graspan_epochs;
      for (NodeId source = 0; source < graph_.vertexCount(); ++source) {
        solveGraspanSource(source, stats);
      }
    }
  }

  void processWorkItem(const WorkItem &item, ReachabilityStats &stats) {
    ++stats.processed_work_items;
    const RelationEdge &selected = item.edge;
    if (item.primary && isDeferredClosureSymbol(selected.symbol)) {
      processPrimaryItem(selected, stats);
      return;
    }

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
        if (usesSpecializedClosure() && isTransitiveRule(rule)) {
          continue;
        }
        if (processTreeForwardJoin(rule, selected, stats)) {
          continue;
        }
        join_candidates_.clear();
        joinRelation().forEachSuccessor(
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
        if (usesSpecializedClosure() && isTransitiveRule(rule)) {
          continue;
        }
        if (processTreeBackwardJoin(rule, selected, stats)) {
          continue;
        }
        join_candidates_.clear();
        joinRelation().forEachPredecessor(
            rule.first, selected.source,
            [&](NodeId source) { join_candidates_.push_back(source); });
        for (NodeId source : join_candidates_) {
          ++stats.classical_iterations;
          addDerived(rule.lhs, source, selected.target, stats);
        }
      }
    }
  }

  bool usesSpecializedClosure() const {
    return transitive_relation_ || pocr_relation_ || fully_ordered_relation_;
  }

  bool isDeferredClosureSymbol(SymbolId symbol) const {
    return (pocr_relation_ && pocr_relation_->isTransitive(symbol)) ||
           (fully_ordered_relation_ &&
            fully_ordered_relation_->isTransitive(symbol));
  }

  bool addPrimaryArc(SymbolId symbol, NodeId source, NodeId target) {
    if (pocr_relation_ && pocr_relation_->isTransitive(symbol)) {
      return pocr_relation_->addPrimaryArc(symbol, source, target);
    }
    if (fully_ordered_relation_ &&
        fully_ordered_relation_->isTransitive(symbol)) {
      return fully_ordered_relation_->addPrimaryArc(symbol, source, target);
    }
    throw std::logic_error("Symbol has no deferred transitive closure");
  }

  std::vector<std::pair<NodeId, NodeId>>
  closePrimaryArc(SymbolId symbol, NodeId source, NodeId target) {
    if (pocr_relation_ && pocr_relation_->isTransitive(symbol)) {
      return pocr_relation_->closePrimaryArc(symbol, source, target);
    }
    if (fully_ordered_relation_ &&
        fully_ordered_relation_->isTransitive(symbol)) {
      return fully_ordered_relation_->closePrimaryArc(symbol, source, target);
    }
    throw std::logic_error("Symbol has no deferred transitive closure");
  }

  void processPrimaryItem(const RelationEdge &edge, ReachabilityStats &stats) {
    const auto discovered =
        closePrimaryArc(edge.symbol, edge.source, edge.target);
    stats.classical_iterations += discovered.size();
    for (const auto &[source, target] : discovered) {
      addCandidate(edge.symbol, source, target, false);
      pushWorkItem({edge.symbol, source, target}, false);
      if (source != edge.source || target != edge.target) {
        ++stats.added_edges;
      }
    }
  }

  bool processTreeForwardJoin(const BinaryRuleId &rule,
                              const RelationEdge &selected,
                              ReachabilityStats &stats) {
    if (rule.lhs != selected.symbol) {
      return false;
    }
    const bool is_pocr =
        pocr_relation_ && pocr_relation_->isTransitive(rule.second);
    auto visit = [&](NodeId target) {
      if (is_pocr) {
        ++stats.pocr_tree_join_visits;
      } else {
        ++stats.fully_ordered_tree_join_visits;
      }
      ++stats.classical_iterations;
      return addDerived(rule.lhs, selected.source, target, stats);
    };
    if (is_pocr) {
      pocr_relation_->closure(rule.second)
          .traverseSuccessorTree(selected.target, visit);
    } else if (fully_ordered_relation_ &&
               fully_ordered_relation_->isTransitive(rule.second)) {
      fully_ordered_relation_->closure(rule.second)
          .traverseCriticalSuccessors(selected.target, visit);
    } else {
      return false;
    }
    return true;
  }

  bool processTreeBackwardJoin(const BinaryRuleId &rule,
                               const RelationEdge &selected,
                               ReachabilityStats &stats) {
    if (rule.lhs != selected.symbol) {
      return false;
    }
    const bool is_pocr =
        pocr_relation_ && pocr_relation_->isTransitive(rule.first);
    auto visit = [&](NodeId source) {
      if (is_pocr) {
        ++stats.pocr_tree_join_visits;
      } else {
        ++stats.fully_ordered_tree_join_visits;
      }
      ++stats.classical_iterations;
      return addDerived(rule.lhs, source, selected.target, stats);
    };
    if (is_pocr) {
      pocr_relation_->closure(rule.first)
          .traversePredecessorTree(selected.source, visit);
    } else if (fully_ordered_relation_ &&
               fully_ordered_relation_->isTransitive(rule.first)) {
      fully_ordered_relation_->closure(rule.first)
          .traverseCriticalPredecessors(selected.source, visit);
    } else {
      return false;
    }
    return true;
  }

  std::vector<std::pair<NodeId, NodeId>>
  addTransitiveArc(SymbolId symbol, NodeId source, NodeId target) {
    if (transitive_relation_ && transitive_relation_->isTransitive(symbol)) {
      return transitive_relation_->addTransitiveArc(symbol, source, target);
    }
    if (pocr_relation_ && pocr_relation_->isTransitive(symbol)) {
      return pocr_relation_->addTransitiveArc(symbol, source, target);
    }
    if (fully_ordered_relation_ &&
        fully_ordered_relation_->isTransitive(symbol)) {
      return fully_ordered_relation_->addTransitiveArc(symbol, source, target);
    }
    throw std::logic_error("Symbol has no specialized transitive closure");
  }

  bool addDerived(SymbolId symbol, NodeId source, NodeId target,
                  ReachabilityStats &stats, bool force_candidate = false) {
    if (backend_ == SolverBackend::EndpointQuotient) {
      if (!eq_engine_->addEdge(symbol, source, target)) {
        ++stats.duplicate_edges;
        return false;
      }
      addCandidate(symbol, source, target, force_candidate);
      ++stats.added_edges;
      return true;
    }
    if (backend_ == SolverBackend::Pearl) {
      if (!pearl_engine_->addEdge(symbol, source, target)) {
        ++stats.duplicate_edges;
        return false;
      }
      addCandidate(symbol, source, target, force_candidate);
      ++stats.added_edges;
      return true;
    }
    if (backend_ == SolverBackend::Sqid) {
      if (!sqid_engine_->addEdge(symbol, source, target)) {
        ++stats.duplicate_edges;
        return false;
      }
      addCandidate(symbol, source, target, force_candidate);
      ++stats.added_edges;
      return true;
    }
    if (backend_ == SolverBackend::Graspan) {
      if (!relation_->add(symbol, source, target)) {
        ++stats.duplicate_edges;
        return false;
      }
      graspan_current_->add(symbol, source, target);
      addCandidate(symbol, source, target, force_candidate);
      current_peak_worklist_size_ =
          std::max(current_peak_worklist_size_, graspan_current_->edgeCount());
      ++stats.added_edges;
      return true;
    }
    if (isDeferredClosureSymbol(symbol)) {
      if (!addPrimaryArc(symbol, source, target)) {
        ++stats.duplicate_edges;
        return false;
      }
      addCandidate(symbol, source, target, force_candidate);
      pushWorkItem({symbol, source, target}, true);
      ++stats.added_edges;
      return true;
    }
    if (transitive_relation_ && transitive_relation_->isTransitive(symbol)) {
      const auto discovered = addTransitiveArc(symbol, source, target);
      if (discovered.empty()) {
        ++stats.duplicate_edges;
        return false;
      }
      stats.classical_iterations += discovered.size();
      for (const auto &[new_source, new_target] : discovered) {
        addCandidate(symbol, new_source, new_target, force_candidate);
        pushWorkItem({symbol, new_source, new_target});
        ++stats.added_edges;
      }
      return true;
    }
    if (!relation_->add(symbol, source, target)) {
      ++stats.duplicate_edges;
      return false;
    }
    addCandidate(symbol, source, target, force_candidate);
    pushWorkItem({symbol, source, target});
    ++stats.added_edges;
    return true;
  }

  TransitiveCounters transitiveCounters() const {
    TransitiveCounters counters;
    auto collect = [&](const auto *relation) {
      if (!relation) {
        return;
      }
      for (const auto &[_, closure] : relation->closures()) {
        const auto &closure_stats = closure->statistics();
        counters.arc_insertions += closure_stats.arc_insertions;
        counters.propagated_pairs += closure_stats.propagated_pairs;
        counters.duplicate_pairs += closure_stats.duplicate_pairs;
      }
    };
    collect(transitive_relation_);
    collect(pocr_relation_);
    collect(fully_ordered_relation_);
    return counters;
  }

  void collectTransitiveStatistics(ReachabilityStats &stats,
                                   const TransitiveCounters &before) const {
    TransitiveCounters after;
    auto collect = [&](const auto *relation) {
      if (!relation) {
        return;
      }
      for (const auto &[_, closure] : relation->closures()) {
        const auto &closure_stats = closure->statistics();
        ++stats.transitive_closure_instances;
        stats.transitive_relation_edges += closure_stats.relation_edges;
        after.arc_insertions += closure_stats.arc_insertions;
        after.propagated_pairs += closure_stats.propagated_pairs;
        after.duplicate_pairs += closure_stats.duplicate_pairs;
        stats.transitive_payload_bytes_estimate +=
            closure->estimatedPayloadBytes();
      }
    };
    collect(transitive_relation_);
    collect(pocr_relation_);
    collect(fully_ordered_relation_);
    stats.transitive_arc_insertions =
        after.arc_insertions - before.arc_insertions;
    stats.transitive_propagated_pairs =
        after.propagated_pairs - before.propagated_pairs;
    stats.transitive_duplicate_pairs =
        after.duplicate_pairs - before.duplicate_pairs;
  }

  PocrCounters pocrCounters() const {
    PocrCounters counters;
    if (!pocr_relation_) {
      return counters;
    }
    for (const auto &[_, closure] : pocr_relation_->closures()) {
      counters.traversal_steps += closure->statistics().traversal_steps;
    }
    return counters;
  }

  void collectPocrStatistics(ReachabilityStats &stats,
                             const PocrCounters &before) const {
    if (!pocr_relation_) {
      return;
    }
    PocrCounters after;
    for (const auto &[_, closure] : pocr_relation_->closures()) {
      const engines::PocrClosureStatistics &closure_stats =
          closure->statistics();
      stats.pocr_tree_roots += closure_stats.tree_roots;
      stats.pocr_tree_nodes += closure_stats.tree_nodes;
      stats.pocr_tree_edges += closure_stats.tree_edges;
      after.traversal_steps += closure_stats.traversal_steps;
    }
    stats.pocr_traversal_steps = after.traversal_steps - before.traversal_steps;
  }

  FullyOrderedCounters fullyOrderedCounters() const {
    FullyOrderedCounters counters;
    if (!fully_ordered_relation_) {
      return counters;
    }
    for (const auto &[_, closure] : fully_ordered_relation_->closures()) {
      const engines::FullyOrderedClosureStatistics &closure_stats =
          closure->statistics();
      counters.reachability_checks += closure_stats.reachability_checks;
      counters.critical_edge_insertions +=
          closure_stats.critical_edge_insertions;
      counters.critical_edge_removals += closure_stats.critical_edge_removals;
      counters.cycle_simplifications += closure_stats.cycle_simplifications;
    }
    return counters;
  }

  void collectFullyOrderedStatistics(ReachabilityStats &stats,
                                     const FullyOrderedCounters &before) const {
    if (!fully_ordered_relation_) {
      return;
    }
    FullyOrderedCounters after;
    for (const auto &[_, closure] : fully_ordered_relation_->closures()) {
      const engines::FullyOrderedClosureStatistics &closure_stats =
          closure->statistics();
      stats.fully_ordered_critical_edges += closure_stats.critical_edges;
      after.reachability_checks += closure_stats.reachability_checks;
      after.critical_edge_insertions += closure_stats.critical_edge_insertions;
      after.critical_edge_removals += closure_stats.critical_edge_removals;
      after.cycle_simplifications += closure_stats.cycle_simplifications;
    }
    stats.fully_ordered_reachability_checks =
        after.reachability_checks - before.reachability_checks;
    stats.fully_ordered_critical_edge_insertions =
        after.critical_edge_insertions - before.critical_edge_insertions;
    stats.fully_ordered_critical_edge_removals =
        after.critical_edge_removals - before.critical_edge_removals;
    stats.fully_ordered_cycle_simplifications =
        after.cycle_simplifications - before.cycle_simplifications;
  }

  bool insertInputFact(SymbolId symbol, NodeId source, NodeId target) {
    if (backend_ == SolverBackend::EndpointQuotient) {
      if (!eq_engine_->addEdge(symbol, source, target)) {
        return false;
      }
      addCandidate(symbol, source, target, true);
      return true;
    }
    if (backend_ == SolverBackend::Pearl) {
      if (!pearl_engine_->addEdge(symbol, source, target)) {
        return false;
      }
      addCandidate(symbol, source, target, true);
      return true;
    }
    if (backend_ == SolverBackend::Sqid) {
      if (!sqid_engine_->addEdge(symbol, source, target)) {
        return false;
      }
      addCandidate(symbol, source, target, true);
      return true;
    }
    if (backend_ == SolverBackend::Graspan) {
      if (!relation_->add(symbol, source, target)) {
        return false;
      }
      graspan_current_->add(symbol, source, target);
      addCandidate(symbol, source, target, true);
      current_peak_worklist_size_ =
          std::max(current_peak_worklist_size_, graspan_current_->edgeCount());
      return true;
    }
    if (isDeferredClosureSymbol(symbol)) {
      if (!addPrimaryArc(symbol, source, target)) {
        return false;
      }
      addCandidate(symbol, source, target, true);
      pushWorkItem({symbol, source, target}, true);
      return true;
    }
    if (transitive_relation_ && transitive_relation_->isTransitive(symbol)) {
      const bool existed = relation_->contains(symbol, source, target);
      const auto discovered = addTransitiveArc(symbol, source, target);
      for (const auto &[new_source, new_target] : discovered) {
        const bool is_input = new_source == source && new_target == target;
        addCandidate(symbol, new_source, new_target, is_input);
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
    addCandidate(symbol, source, target, true);
    pushWorkItem({symbol, source, target});
    return true;
  }

  void pushWorkItem(const RelationEdge &edge, bool primary = false) {
    const WorkItem item{edge, primary};
    if (backend_ == SolverBackend::HierarchicalPocr && primary &&
        grammar_.transitiveSymbols().count(edge.symbol) != 0) {
      primary_worklist_.push_back(item);
    }
    worklist_.push_back(item);
    current_peak_worklist_size_ =
        std::max(current_peak_worklist_size_,
                 primary_worklist_.size() + worklist_.size());
  }

  bool isCandidateSymbol(SymbolId symbol) const {
    const std::string &name = grammar_.symbolName(symbol);
    return grammar_.isTerminal(name) || grammar_.isInsertSymbol(name);
  }

  void addCandidate(SymbolId symbol, NodeId source, NodeId target, bool force) {
    if (candidate_relation_ && (force || isCandidateSymbol(symbol))) {
      candidate_relation_->add(symbol, source, target);
    }
  }

  const Relation &joinRelation() const {
    return candidate_relation_ ? *candidate_relation_ : *relation_;
  }

  LabeledGraph &graph_;
  const Grammar &grammar_;
  SolverBackend backend_;
  bool unidirectional_ = false;
  std::unique_ptr<Relation> relation_;
  std::unique_ptr<Relation> candidate_relation_;
  std::deque<WorkItem> worklist_;
  std::deque<WorkItem> primary_worklist_;
  std::vector<NodeId> join_candidates_;
  std::unique_ptr<GraspanData> graspan_old_;
  std::unique_ptr<GraspanData> graspan_current_;
  std::unique_ptr<engines::SqidEngine> sqid_engine_;
  std::unique_ptr<engines::PearlEngine> pearl_engine_;
  std::unique_ptr<engines::EndpointQuotientEngine> eq_engine_;
  std::size_t input_edges_ = 0;
  std::size_t current_peak_worklist_size_ = 0;
  std::size_t pending_derived_edges_ = 0;
  std::size_t nullable_seeded_nodes_ = 0;
  BitVectorClosureRelation *transitive_relation_ = nullptr;
  PocrClosureRelation *pocr_relation_ = nullptr;
  FullyOrderedClosureRelation *fully_ordered_relation_ = nullptr;
  std::uint64_t expected_graph_version_ = 0;
};

SolverSession::SolverSession(LabeledGraph &graph, const Grammar &grammar,
                             SolverBackend backend)
    : SolverSession(graph, grammar, SolverOptions{backend, false, false, {}}) {}

SolverSession::SolverSession(LabeledGraph &graph, const Grammar &grammar,
                             const SolverOptions &options)
    : impl_(std::make_unique<Impl>(graph, grammar, options)) {}

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
