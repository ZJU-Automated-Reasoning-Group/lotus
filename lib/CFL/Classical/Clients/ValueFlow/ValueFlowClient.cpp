#include "CFL/Classical/Clients/ValueFlow/ValueFlowClient.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <llvm/IR/InstIterator.h>

namespace lotus::cfl::classical {
namespace {

std::string nodeName(std::size_t id) { return std::to_string(id); }

ReachabilityStats
specializedStats(const engines::SpecializedPocrStatistics &source,
                 const Grammar &grammar) {
  ReachabilityStats result;
  result.graph_nodes = source.graph_nodes;
  result.base_graph_edges = source.graph_edges;
  result.grammar_symbols = grammar.symbolCount();
  result.grammar_terminals = grammar.terminals().size();
  result.grammar_nonterminals = grammar.nonterminals().size();
  result.grammar_productions = grammar.productionCount();
  result.grammar_nullable_symbols = grammar.nullableSymbols().size();
  result.grammar_transitive_symbols = grammar.transitiveSymbols().size();
  result.input_edges = source.graph_edges;
  result.relation_edges = source.reachability_pairs;
  result.start_symbol_edges = source.value_or_flow_pairs;
  result.classical_iterations = source.reachability_checks;
  result.processed_work_items = source.processed_items;
  result.duplicate_edges = source.duplicate_items;
  result.added_edges = source.reachability_pairs;
  result.specialized_reachability_pairs = source.reachability_pairs;
  result.specialized_matched_pairs = source.matched_pairs;
  result.specialized_critical_edges = source.critical_edges;
  result.fully_ordered_cycle_simplifications = source.cycle_simplifications;
  return result;
}

void addBidirectionalEdge(LabeledGraph &graph, std::size_t source,
                          std::size_t target, const std::string &forward,
                          const std::string &reverse) {
  graph.addEdge(source, target, forward);
  graph.addEdge(target, source, reverse);
}

std::string encodeCallLabel(const std::string &base, std::uint32_t id) {
  return base + '_' + std::to_string(id);
}

struct CallSiteKey {
  const llvm::CallBase *call_site = nullptr;
  const llvm::Function *callee = nullptr;

  bool operator==(const CallSiteKey &other) const {
    return call_site == other.call_site && callee == other.callee;
  }
};

struct CallSiteKeyHash {
  std::size_t operator()(const CallSiteKey &key) const {
    const std::size_t call = std::hash<const llvm::CallBase *>{}(key.call_site);
    const std::size_t callee = std::hash<const llvm::Function *>{}(key.callee);
    return call ^ (callee + (call << 6U) + (call >> 2U));
  }
};

CallSiteKey callSiteKey(const lotus::analysis::SVFGEdge *edge) {
  if (!edge || !edge->getCallSite()) {
    throw std::invalid_argument("Call/return SVFG edge has no callsite");
  }
  const llvm::Function *callee = nullptr;
  if (edge->isCallEdge()) {
    callee = edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
  } else if (edge->isRetEdge()) {
    callee = edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
  }
  return {edge->getCallSite(), callee};
}

std::size_t instructionOrdinal(const llvm::CallBase *call_site) {
  const llvm::Function *function = call_site->getFunction();
  if (!function) {
    throw std::invalid_argument("SVFG callsite is detached from a function");
  }
  std::size_t ordinal = 0;
  for (const llvm::Instruction &instruction : llvm::instructions(*function)) {
    if (&instruction == call_site) {
      return ordinal;
    }
    ++ordinal;
  }
  throw std::invalid_argument("SVFG callsite is absent from its function");
}

using CallSiteIds =
    std::unordered_map<CallSiteKey, std::uint32_t, CallSiteKeyHash>;

CallSiteIds buildCallSiteIds(const lotus::analysis::SVFG &svfg) {
  std::vector<CallSiteKey> keys;
  std::unordered_set<CallSiteKey, CallSiteKeyHash> seen;
  for (const auto &[_, node] : svfg) {
    for (const lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || (!edge->isCallEdge() && !edge->isRetEdge())) {
        continue;
      }
      const CallSiteKey key = callSiteKey(edge);
      if (seen.insert(key).second) {
        keys.push_back(key);
      }
    }
  }

  std::sort(keys.begin(), keys.end(),
            [](const CallSiteKey &lhs, const CallSiteKey &rhs) {
              const std::string lhs_caller =
                  lhs.call_site->getFunction()->getName().str();
              const std::string rhs_caller =
                  rhs.call_site->getFunction()->getName().str();
              if (lhs_caller != rhs_caller) {
                return lhs_caller < rhs_caller;
              }
              const std::size_t lhs_ordinal = instructionOrdinal(lhs.call_site);
              const std::size_t rhs_ordinal = instructionOrdinal(rhs.call_site);
              if (lhs_ordinal != rhs_ordinal) {
                return lhs_ordinal < rhs_ordinal;
              }
              const std::string lhs_callee =
                  lhs.callee ? lhs.callee->getName().str() : std::string();
              const std::string rhs_callee =
                  rhs.callee ? rhs.callee->getName().str() : std::string();
              return lhs_callee < rhs_callee;
            });

  CallSiteIds ids;
  std::unordered_set<std::uint32_t> used_ids;
  for (const CallSiteKey &key : keys) {
    if (!key.callee) {
      continue;
    }
    const std::uint32_t registered =
        svfg.getCallSiteId(key.call_site, key.callee);
    if (registered == 0) {
      continue;
    }
    if (!used_ids.insert(registered).second) {
      throw std::invalid_argument("SVFG callsite IDs are not unique");
    }
    ids.emplace(key, registered);
  }

  std::uint32_t next_id = 1;
  for (const CallSiteKey &key : keys) {
    if (ids.count(key) != 0) {
      continue;
    }
    while (used_ids.count(next_id) != 0) {
      ++next_id;
    }
    ids.emplace(key, next_id);
    used_ids.insert(next_id);
    ++next_id;
  }
  return ids;
}

std::uint32_t callSiteId(const CallSiteIds &ids,
                         const lotus::analysis::SVFGEdge *edge) {
  return ids.at(callSiteKey(edge));
}

} // namespace

struct ValueFlowClient::State {
  State(LabeledGraph input_graph, Grammar input_grammar)
      : graph(std::move(input_graph)), grammar(std::move(input_grammar)) {}

  LabeledGraph graph;
  Grammar grammar;
};

LabeledGraph encodeSVFG(const lotus::analysis::SVFG &svfg) {
  LabeledGraph encoded;
  for (const auto &[node_id, _] : svfg) {
    encoded.addVertex(nodeName(node_id));
  }
  const CallSiteIds callsite_ids = buildCallSiteIds(svfg);

  for (const auto &[_, node] : svfg) {
    for (lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
      if (!edge) {
        continue;
      }

      const std::size_t source =
          encoded.vertexId(nodeName(edge->getSrcNode()->getId()));
      const std::size_t target =
          encoded.vertexId(nodeName(edge->getDstNode()->getId()));

      if (edge->isCallEdge()) {
        const std::uint32_t id = callSiteId(callsite_ids, edge);
        addBidirectionalEdge(encoded, source, target,
                             encodeCallLabel("call", id),
                             encodeCallLabel("callbar", id));
        continue;
      }

      if (edge->isRetEdge()) {
        const std::uint32_t id = callSiteId(callsite_ids, edge);
        addBidirectionalEdge(encoded, source, target,
                             encodeCallLabel("ret", id),
                             encodeCallLabel("retbar", id));
        continue;
      }

      if (lotus::analysis::isThreadMHPVFGEdge(edge->getEdgeKind())) {
        addBidirectionalEdge(encoded, source, target, "thread", "threadbar");
      } else if (lotus::analysis::isIndirectVFGEdge(edge->getEdgeKind())) {
        addBidirectionalEdge(encoded, source, target, "indirect",
                             "indirectbar");
      } else if (lotus::analysis::isDirectVFGEdge(edge->getEdgeKind())) {
        addBidirectionalEdge(encoded, source, target, "direct", "directbar");
      } else {
        throw std::invalid_argument(
            "Unsupported SVFG edge in CFL value-flow encoding: " +
            edge->toString());
      }
    }
  }
  return encoded;
}

Grammar buildVfgGrammar(const lotus::analysis::SVFG &svfg) {
  std::set<std::uint32_t> callsite_ids;
  const CallSiteIds ids = buildCallSiteIds(svfg);

  for (const auto &[_, node] : svfg) {
    for (lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || (!edge->isCallEdge() && !edge->isRetEdge())) {
        continue;
      }
      callsite_ids.insert(callSiteId(ids, edge));
    }
  }

  GrammarParseOptions options;
  options.variable_attributes['i'].assign(callsite_ids.begin(),
                                          callsite_ids.end());
  if (options.variable_attributes['i'].empty()) {
    return Grammar::parseFromText("Start:\n"
                                  "  A\n"
                                  "Terminal:\n"
                                  "  direct directbar indirect indirectbar "
                                  "thread threadbar\n"
                                  "Variables:\n"
                                  "  A Abar\n"
                                  "Productions:\n"
                                  "  A -> A A | direct | indirect | thread | "
                                  "<epsilon>;\n"
                                  "  Abar -> Abar Abar | directbar | "
                                  "indirectbar | threadbar | <epsilon>;\n");
  }
  return Grammar::parseFromText(
      "Start:\n"
      "  A\n"
      "Terminal:\n"
      "  direct directbar indirect indirectbar thread threadbar call ret "
      "callbar retbar\n"
      "Variables:\n"
      "  A Abar\n"
      "Productions:\n"
      "  A -> A A | direct | indirect | thread | call_i A ret_i | "
      "<epsilon>;\n"
      "  Abar -> Abar Abar | directbar | indirectbar | threadbar | "
      "retbar_i Abar callbar_i | <epsilon>;\n",
      options);
}

ValueFlowClient ValueFlowClient::fromSVFG(const lotus::analysis::SVFG &svfg) {
  LabeledGraph graph = encodeSVFG(svfg);
  Grammar grammar = buildVfgGrammar(svfg);
  std::unordered_map<std::uint32_t, std::size_t> node_to_vertex;
  for (const auto &[node_id, _] : svfg) {
    node_to_vertex.emplace(node_id, graph.vertexId(nodeName(node_id)));
  }
  return ValueFlowClient(std::move(graph), std::move(grammar),
                         std::move(node_to_vertex));
}

ValueFlowClient
ValueFlowClient::fromPreparedSVFG(lotus::analysis::SVFG &svfg,
                                  const SVFGPreparationOptions &options) {
  prepareSVFGForCFL(svfg, options);
  return fromSVFG(svfg);
}

ValueFlowClient::ValueFlowClient(
    LabeledGraph graph, Grammar grammar,
    std::unordered_map<std::uint32_t, std::size_t> node_to_vertex)
    : state_(std::make_unique<State>(std::move(graph), std::move(grammar))),
      node_to_vertex_(std::move(node_to_vertex)),
      vertex_to_node_(state_->graph.vertexCount()) {
  for (const auto &[node, vertex] : node_to_vertex_) {
    vertex_to_node_.at(vertex) = node;
  }
}

ValueFlowClient::~ValueFlowClient() = default;

ValueFlowClient::ValueFlowClient(ValueFlowClient &&other) noexcept
    : state_(std::move(other.state_)),
      node_to_vertex_(std::move(other.node_to_vertex_)),
      vertex_to_node_(std::move(other.vertex_to_node_)),
      session_(std::move(other.session_)), backend_(std::move(other.backend_)),
      pocr_engine_(std::move(other.pocr_engine_)),
      focr_engine_(std::move(other.focr_engine_)),
      specialized_backend_(std::move(other.specialized_backend_)),
      specialized_focr_cycles_(other.specialized_focr_cycles_) {}

ValueFlowClient &ValueFlowClient::operator=(ValueFlowClient &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  session_.reset();
  state_ = std::move(other.state_);
  node_to_vertex_ = std::move(other.node_to_vertex_);
  vertex_to_node_ = std::move(other.vertex_to_node_);
  session_ = std::move(other.session_);
  backend_ = std::move(other.backend_);
  pocr_engine_ = std::move(other.pocr_engine_);
  focr_engine_ = std::move(other.focr_engine_);
  specialized_backend_ = std::move(other.specialized_backend_);
  specialized_focr_cycles_ = other.specialized_focr_cycles_;
  return *this;
}

ReachabilityStats ValueFlowClient::solve(SolverBackend backend) {
  if (specialized_backend_) {
    throw std::invalid_argument(
        "Cannot switch from a specialized value-flow engine to SolverSession");
  }
  if (backend_ && *backend_ != backend) {
    throw std::invalid_argument(
        "Cannot change solver backend after a value-flow session has started");
  }
  if (!session_) {
    session_ = std::make_unique<SolverSession>(state_->graph, state_->grammar,
                                               backend);
    backend_ = backend;
  }
  return session_->solve();
}

ReachabilityStats
ValueFlowClient::solveSpecialized(engines::SpecializedPocrBackend backend,
                                  bool simplify_focr_cycles) {
  const auto start = std::chrono::steady_clock::now();
  simplify_focr_cycles =
      backend == engines::SpecializedPocrBackend::Focr && simplify_focr_cycles;
  if (session_) {
    throw std::invalid_argument(
        "Cannot switch from SolverSession to a specialized value-flow engine");
  }
  if (specialized_backend_ && *specialized_backend_ != backend) {
    throw std::invalid_argument(
        "Cannot change specialized value-flow engine after solving started");
  }
  if (specialized_backend_ &&
      specialized_focr_cycles_ != simplify_focr_cycles) {
    throw std::invalid_argument(
        "Cannot change FOCR cycle simplification after solving has started");
  }
  specialized_backend_ = backend;
  specialized_focr_cycles_ = simplify_focr_cycles;
  if (backend == engines::SpecializedPocrBackend::Pocr) {
    if (!pocr_engine_) {
      pocr_engine_ =
          std::make_unique<engines::PocrValueFlowEngine>(state_->graph);
    }
    ReachabilityStats result =
        specializedStats(pocr_engine_->solve(), state_->grammar);
    result.solve_time_microseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    return result;
  }
  if (!focr_engine_) {
    focr_engine_ = std::make_unique<engines::FocrValueFlowEngine>(
        state_->graph, simplify_focr_cycles);
  }
  ReachabilityStats result =
      specializedStats(focr_engine_->solve(), state_->grammar);
  result.solve_time_microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  return result;
}

bool ValueFlowClient::hasFlow(std::uint32_t source_node,
                              std::uint32_t target_node) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  const auto source_it = node_to_vertex_.find(source_node);
  const auto target_it = node_to_vertex_.find(target_node);
  if (source_it == node_to_vertex_.end() ||
      target_it == node_to_vertex_.end()) {
    return false;
  }
  if (specialized_backend_) {
    if (*specialized_backend_ == engines::SpecializedPocrBackend::Pocr) {
      return pocr_engine_->hasFlow(source_it->second, target_it->second);
    }
    return focr_engine_->hasFlow(source_it->second, target_it->second);
  }
  return session_->contains(source_it->second, target_it->second, "A");
}

std::vector<std::uint32_t>
ValueFlowClient::reachableFrom(std::uint32_t source_node) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  const auto source_it = node_to_vertex_.find(source_node);
  if (source_it == node_to_vertex_.end()) {
    return {};
  }

  std::vector<std::uint32_t> reachable;
  auto add_vertex = [&](NodeId vertex) {
    if (vertex >= vertex_to_node_.size()) {
      return;
    }
    if (const auto node = vertex_to_node_[vertex]) {
      reachable.push_back(*node);
    }
  };
  if (specialized_backend_) {
    const auto pairs =
        *specialized_backend_ == engines::SpecializedPocrBackend::Pocr
            ? pocr_engine_->flowPairs()
            : focr_engine_->flowPairs();
    for (const auto &[source, target] : pairs) {
      if (source == source_it->second) {
        add_vertex(target);
      }
    }
  } else {
    const SymbolId flow_symbol = state_->grammar.symbolId("A");
    session_->relation().forEachSuccessor(flow_symbol, source_it->second,
                                          add_vertex);
  }
  std::sort(reachable.begin(), reachable.end());
  return reachable;
}

const LabeledGraph &ValueFlowClient::graph() const { return state_->graph; }

const Grammar &ValueFlowClient::grammar() const { return state_->grammar; }

} // namespace lotus::cfl::classical
