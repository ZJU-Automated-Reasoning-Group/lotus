#include "CFL/Classical/Solvers/Engines/STG/StagedSolver.h"

#include "CFL/Classical/Solvers/Engines/TransitiveClosure.h"
#include "Utils/ADT/TarjanScc.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include <llvm/ADT/SparseBitVector.h>

namespace lotus::cfl::classical::engines::stg {
namespace {

using BitVector = llvm::SparseBitVector<>;
using IncomingMatrix = std::vector<BitVector>;

IncomingMatrix identityMatrix(std::size_t node_count) {
  IncomingMatrix result(node_count);
  for (NodeId node = 0; node < node_count; ++node) {
    result[node].set(static_cast<unsigned>(node));
  }
  return result;
}

IncomingMatrix applyLiteral(const Relation &relation,
                            const IncomingMatrix &input,
                            const std::vector<SymbolId> &symbols,
                            std::size_t node_count) {
  IncomingMatrix result(node_count);
  for (SymbolId symbol : symbols) {
    for (const RelationEdge &edge : relation.edges(symbol)) {
      result[edge.target] |= input[edge.source];
    }
  }
  return result;
}

IncomingMatrix applyKleene(const Relation &relation,
                           const IncomingMatrix &input,
                           const std::vector<SymbolId> &symbols,
                           std::size_t node_count,
                           StagedStatistics &statistics) {
  std::vector<std::vector<NodeId>> successors(node_count);
  for (SymbolId symbol : symbols) {
    for (const RelationEdge &edge : relation.edges(symbol)) {
      successors[edge.source].push_back(edge.target);
    }
  }
  for (auto &targets : successors) {
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  }

  auto successor_range = [&](NodeId node) -> const auto & {
    return successors[node];
  };
  std::vector<NodeId> component;
  std::vector<NodeId> reverse_topological_order;
  const std::size_t component_count = FindStronglyConnectedComponents(
      node_count, successor_range, component, reverse_topological_order);

  std::vector<BitVector> component_values(component_count);
  std::vector<std::set<NodeId>> component_successors(component_count);
  for (NodeId node = 0; node < node_count; ++node) {
    component_values[component[node]] |= input[node];
    for (NodeId target : successors[node]) {
      if (component[node] != component[target]) {
        component_successors[component[node]].insert(component[target]);
      }
    }
  }

  // Tarjan assigns source SCCs larger indices than their successors.
  for (std::size_t index = component_count; index > 0; --index) {
    const NodeId source_component = index - 1;
    for (NodeId target_component : component_successors[source_component]) {
      component_values[target_component] |= component_values[source_component];
      ++statistics.ordered_scc_propagations;
    }
  }

  IncomingMatrix result(node_count);
  for (NodeId node = 0; node < node_count; ++node) {
    result[node] = component_values[component[node]];
  }
  return result;
}

IncomingMatrix evaluate(const Relation &relation,
                        const RegularSequence &sequence, std::size_t node_count,
                        StagedStatistics &statistics) {
  IncomingMatrix current = identityMatrix(node_count);
  for (const RegularAtom &atom : sequence) {
    current = atom.kleene_star
                  ? applyKleene(relation, current, atom.symbols, node_count,
                                statistics)
                  : applyLiteral(relation, current, atom.symbols, node_count);
  }
  return current;
}

std::size_t materialize(const RegularProduction &production, Relation &relation,
                        std::size_t node_count, StagedStatistics &statistics) {
  std::size_t added = 0;
  for (const RegularSequence &sequence : production.alternatives) {
    const IncomingMatrix result =
        evaluate(relation, sequence, node_count, statistics);
    for (NodeId target = 0; target < node_count; ++target) {
      for (unsigned source : result[target]) {
        added += relation.add(production.lhs, source, target) ? 1 : 0;
      }
    }
  }
  return added;
}

class BinaryRelation {
public:
  explicit BinaryRelation(std::size_t node_count = 0) {
    ensureNodeCount(node_count);
  }

  void ensureNodeCount(std::size_t node_count) {
    successors_.resize(node_count);
    predecessors_.resize(node_count);
  }

  bool add(NodeId source, NodeId target) {
    if (!successors_.at(source).test_and_set(static_cast<unsigned>(target))) {
      return false;
    }
    predecessors_.at(target).set(static_cast<unsigned>(source));
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
    for (NodeId source = 0; source < successors_.size(); ++source) {
      for (unsigned target : successors_[source]) {
        result.emplace_back(source, target);
      }
    }
    return result;
  }

private:
  std::vector<BitVector> successors_;
  std::vector<BitVector> predecessors_;
};

class DyckComponent {
public:
  DyckComponent(const DyckCfp &specification, Relation &relation,
                std::size_t node_count)
      : specification_(specification), relation_(relation),
        closure_(node_count), node_count_(node_count) {}

  void ensureNodeCount(std::size_t node_count) {
    closure_.ensureNodeCount(node_count);
    node_count_ = node_count;
  }

  std::size_t solve(StagedStatistics &statistics) {
    std::deque<std::pair<NodeId, NodeId>> path_worklist;
    for (NodeId node = 0; node < node_count_; ++node) {
      path_worklist.emplace_back(node, node);
    }

    auto addBodySymbol = [&](SymbolId symbol) {
      for (const RelationEdge &edge : relation_.edges(symbol)) {
        for (const auto &pair : closure_.addArc(edge.source, edge.target)) {
          path_worklist.push_back(pair);
          ++statistics.dyck_path_edges;
        }
      }
    };
    for (SymbolId symbol : specification_.body_symbols) {
      addBodySymbol(symbol);
    }
    addBodySymbol(specification_.summary);
    for (const auto &pair : closure_.edges()) {
      path_worklist.push_back(pair);
    }

    std::size_t added = 0;
    while (!path_worklist.empty()) {
      const auto [inner_source, inner_target] = path_worklist.front();
      path_worklist.pop_front();
      for (const DelimiterPair &delimiter : specification_.delimiters) {
        std::vector<NodeId> sources;
        std::vector<NodeId> targets;
        relation_.forEachPredecessor(
            delimiter.open, inner_source,
            [&](NodeId source) { sources.push_back(source); });
        relation_.forEachSuccessor(
            delimiter.close, inner_target,
            [&](NodeId target) { targets.push_back(target); });
        for (NodeId source : sources) {
          for (NodeId target : targets) {
            if (!relation_.add(specification_.summary, source, target)) {
              continue;
            }
            ++added;
            ++statistics.summary_edges;
            for (const auto &pair : closure_.addArc(source, target)) {
              path_worklist.push_back(pair);
              ++statistics.dyck_path_edges;
            }
          }
        }
      }
    }
    return added;
  }

private:
  DyckCfp specification_;
  Relation &relation_;
  IncrementalTransitiveClosure closure_;
  std::size_t node_count_ = 0;
};

class AliasComponent {
public:
  AliasComponent(const AliasCfp &specification, Relation &relation,
                 std::size_t node_count)
      : specification_(specification), relation_(relation),
        forward_(node_count), backward_(node_count), seen_forward_(node_count),
        seen_center_(node_count), seen_backward_(node_count) {}

  void ensureNodeCount(std::size_t node_count) {
    forward_.ensureNodeCount(node_count);
    backward_.ensureNodeCount(node_count);
    seen_forward_.ensureNodeCount(node_count);
    seen_center_.ensureNodeCount(node_count);
    seen_backward_.ensureNodeCount(node_count);
  }

  std::size_t solve(StagedStatistics &statistics) {
    synchronizeInputs(statistics);
    forward_worklist_.clear();
    backward_worklist_.clear();
    for (const auto &edge : forward_.edges()) {
      forward_worklist_.push_back(edge);
    }
    for (const auto &edge : backward_.edges()) {
      backward_worklist_.push_back(edge);
    }
    std::size_t added = 0;
    while (!forward_worklist_.empty() || !backward_worklist_.empty()) {
      while (!forward_worklist_.empty()) {
        const auto [source, target] = forward_worklist_.front();
        forward_worklist_.pop_front();

        bool has_close = false;
        relation_.forEachSuccessor(specification_.close, target,
                                   [&](NodeId) { has_close = true; });
        if (has_close) {
          addBackward(target, source, statistics);
        }
        for (unsigned next : seen_backward_.successors(target)) {
          addForward(source, next, statistics);
        }
      }

      while (!backward_worklist_.empty()) {
        const auto [source, target] = backward_worklist_.front();
        backward_worklist_.pop_front();

        std::vector<NodeId> opens;
        std::vector<NodeId> closes;
        relation_.forEachPredecessor(
            specification_.open, target,
            [&](NodeId predecessor) { opens.push_back(predecessor); });
        relation_.forEachSuccessor(
            specification_.close, source,
            [&](NodeId successor) { closes.push_back(successor); });
        for (NodeId open_source : opens) {
          for (NodeId close_target : closes) {
            if (relation_.add(specification_.summary, open_source,
                              close_target)) {
              ++added;
              ++statistics.summary_edges;
            }
          }
        }
        for (unsigned next : seen_forward_.successors(target)) {
          addBackward(source, next, statistics);
        }
      }
    }
    return added;
  }

private:
  void addForward(NodeId source, NodeId target, StagedStatistics &statistics) {
    if (forward_.add(source, target)) {
      forward_worklist_.emplace_back(source, target);
      ++statistics.alias_forward_path_edges;
    }
  }

  void addBackward(NodeId source, NodeId target, StagedStatistics &statistics) {
    if (backward_.add(source, target)) {
      backward_worklist_.emplace_back(source, target);
      ++statistics.alias_backward_path_edges;
    }
  }

  void synchronizeInputs(StagedStatistics &statistics) {
    for (const RelationEdge &edge :
         relation_.edges(specification_.reverse_forward)) {
      if (seen_forward_.add(edge.source, edge.target)) {
        for (unsigned path_source : backward_.predecessors(edge.source)) {
          addBackward(path_source, edge.target, statistics);
        }
      }
    }
    for (const RelationEdge &edge : relation_.edges(specification_.center)) {
      if (seen_center_.add(edge.source, edge.target)) {
        addForward(edge.source, edge.target, statistics);
      }
    }
    for (const RelationEdge &edge : relation_.edges(specification_.backward)) {
      if (seen_backward_.add(edge.source, edge.target)) {
        for (unsigned predecessor : forward_.predecessors(edge.source)) {
          addForward(predecessor, edge.target, statistics);
        }
      }
    }
  }

  AliasCfp specification_;
  Relation &relation_;
  BinaryRelation forward_;
  BinaryRelation backward_;
  BinaryRelation seen_forward_;
  BinaryRelation seen_center_;
  BinaryRelation seen_backward_;
  std::deque<std::pair<NodeId, NodeId>> forward_worklist_;
  std::deque<std::pair<NodeId, NodeId>> backward_worklist_;
};

} // namespace

class StagedSolver::Impl {
public:
  Impl(const Grammar &grammar, Relation &relation,
       StagedSpecification specification, std::size_t node_count)
      : grammar_(grammar), relation_(relation),
        specification_(std::move(specification)), node_count_(node_count) {
    validateSpecification();
    for (const DyckCfp &pattern : specification_.dyck_patterns) {
      dyck_components_.emplace_back(pattern, relation_, node_count_);
    }
    for (const AliasCfp &pattern : specification_.alias_patterns) {
      alias_components_.emplace_back(pattern, relation_, node_count_);
    }
    ensureNodeCount(node_count_);
  }

  void ensureNodeCount(std::size_t node_count) {
    if (node_count > std::numeric_limits<unsigned>::max()) {
      throw std::overflow_error("Stg node count exceeds bitvector range");
    }
    node_count_ = std::max(node_count_, node_count);
    relation_.ensureNodeCount(node_count_);
    for (DyckComponent &component : dyck_components_) {
      component.ensureNodeCount(node_count_);
    }
    for (AliasComponent &component : alias_components_) {
      component.ensureNodeCount(node_count_);
    }
  }

  bool addEdge(SymbolId symbol, NodeId source, NodeId target) {
    if (symbol >= grammar_.symbolCount() || source >= node_count_ ||
        target >= node_count_) {
      throw std::out_of_range("Stg edge is out of range");
    }
    return relation_.add(symbol, source, target);
  }

  StagedStatistics solve() {
    StagedStatistics statistics;
    std::size_t changed = 0;
    do {
      changed = 0;
      ++statistics.phase_l_rounds;
      for (const RegularProduction &production :
           specification_.phase_l_regular) {
        const std::size_t added =
            materialize(production, relation_, node_count_, statistics);
        statistics.phase_l_regular_edges += added;
        changed += added;
      }
      for (DyckComponent &component : dyck_components_) {
        changed += component.solve(statistics);
      }
      for (AliasComponent &component : alias_components_) {
        changed += component.solve(statistics);
      }
    } while (changed != 0);

    for (const RegularProduction &production : specification_.phase_r) {
      ++statistics.phase_r_productions;
      statistics.phase_r_edges +=
          materialize(production, relation_, node_count_, statistics);
    }
    return statistics;
  }

private:
  void validateSymbol(SymbolId symbol) const {
    if (symbol >= grammar_.symbolCount()) {
      throw std::invalid_argument("Stg specification uses unknown symbol");
    }
  }

  void validateRegular(const RegularProduction &production) const {
    validateSymbol(production.lhs);
    for (const RegularSequence &sequence : production.alternatives) {
      for (const RegularAtom &atom : sequence) {
        for (SymbolId symbol : atom.symbols) {
          validateSymbol(symbol);
        }
      }
    }
  }

  void validateSpecification() const {
    for (const RegularProduction &production : specification_.phase_l_regular) {
      validateRegular(production);
    }
    for (const RegularProduction &production : specification_.phase_r) {
      validateRegular(production);
    }
    for (const DyckCfp &pattern : specification_.dyck_patterns) {
      validateSymbol(pattern.summary);
      for (SymbolId symbol : pattern.body_symbols) {
        validateSymbol(symbol);
      }
      for (const DelimiterPair &delimiter : pattern.delimiters) {
        validateSymbol(delimiter.open);
        validateSymbol(delimiter.close);
      }
    }
    for (const AliasCfp &pattern : specification_.alias_patterns) {
      validateSymbol(pattern.summary);
      validateSymbol(pattern.open);
      validateSymbol(pattern.close);
      validateSymbol(pattern.reverse_forward);
      validateSymbol(pattern.center);
      validateSymbol(pattern.backward);
    }
  }

  const Grammar &grammar_;
  Relation &relation_;
  StagedSpecification specification_;
  std::size_t node_count_ = 0;
  std::vector<DyckComponent> dyck_components_;
  std::vector<AliasComponent> alias_components_;
};

StagedSolver::StagedSolver(const Grammar &grammar, Relation &relation,
                           StagedSpecification specification,
                           std::size_t node_count)
    : impl_(std::make_unique<Impl>(grammar, relation, std::move(specification),
                                   node_count)) {}

StagedSolver::~StagedSolver() = default;
StagedSolver::StagedSolver(StagedSolver &&) noexcept = default;
StagedSolver &StagedSolver::operator=(StagedSolver &&) noexcept = default;

void StagedSolver::ensureNodeCount(std::size_t node_count) {
  impl_->ensureNodeCount(node_count);
}

bool StagedSolver::addEdge(SymbolId symbol, NodeId source, NodeId target) {
  return impl_->addEdge(symbol, source, target);
}

StagedStatistics StagedSolver::solve() { return impl_->solve(); }

StagedSpecification
decomposeStandardDyck(SymbolId start, SymbolId summary,
                      std::vector<SymbolId> neutral_symbols,
                      std::vector<DelimiterPair> delimiters) {
  StagedSpecification result;
  result.dyck_patterns.push_back(
      {summary, std::move(delimiters), neutral_symbols});
  neutral_symbols.push_back(summary);
  result.phase_r.push_back({start, {{{std::move(neutral_symbols), true}}}});
  return result;
}

StagedSpecification
decomposeExtendedDyck(SymbolId start, SymbolId summary,
                      std::vector<SymbolId> neutral_symbols,
                      std::vector<DelimiterPair> delimiters) {
  StagedSpecification result;
  std::vector<SymbolId> first = neutral_symbols;
  std::vector<SymbolId> second = neutral_symbols;
  first.push_back(summary);
  second.push_back(summary);
  for (const DelimiterPair &delimiter : delimiters) {
    first.push_back(delimiter.close);
    second.push_back(delimiter.open);
  }
  result.dyck_patterns.push_back(
      {summary, std::move(delimiters), std::move(neutral_symbols)});
  result.phase_r.push_back(
      {start, {{{std::move(first), true}, {std::move(second), true}}}});
  return result;
}

StagedSpecification
decomposeAliasCfp(AliasCfp pattern,
                  std::vector<RegularProduction> phase_l_regular,
                  std::vector<RegularProduction> phase_r) {
  StagedSpecification result;
  result.phase_l_regular = std::move(phase_l_regular);
  result.alias_patterns.push_back(pattern);
  result.phase_r = std::move(phase_r);
  return result;
}

} // namespace lotus::cfl::classical::engines::stg
