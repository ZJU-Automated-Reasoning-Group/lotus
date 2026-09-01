#include "CFL/Classical/SVFGAdapter.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include <llvm/IR/InstIterator.h>

namespace lotus::cfl::classical {
namespace {

std::string nodeName(std::size_t id) { return std::to_string(id); }

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

      if (lotus::analysis::isDirectVFGEdge(edge->getEdgeKind()) ||
          lotus::analysis::isIndirectVFGEdge(edge->getEdgeKind()) ||
          lotus::analysis::isThreadMHPVFGEdge(edge->getEdgeKind())) {
        addBidirectionalEdge(encoded, source, target, "a", "abar");
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
                                  "  a abar\n"
                                  "Variables:\n"
                                  "  A Abar\n"
                                  "Productions:\n"
                                  "  A -> A A | a | epsilon;\n"
                                  "  Abar -> Abar Abar | abar | epsilon;\n");
  }
  return Grammar::parseFromText(
      "Start:\n"
      "  A\n"
      "Terminal:\n"
      "  a abar call ret callbar retbar\n"
      "Variables:\n"
      "  A Abar\n"
      "Productions:\n"
      "  A -> A A | a | call_i A ret_i | epsilon;\n"
      "  Abar -> Abar Abar | abar | retbar_i Abar callbar_i | epsilon;\n",
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

ValueFlowClient::ValueFlowClient(ValueFlowClient &&other) noexcept
    : graph_(std::move(other.graph_)), grammar_(std::move(other.grammar_)),
      node_to_vertex_(std::move(other.node_to_vertex_)) {}

ValueFlowClient &ValueFlowClient::operator=(ValueFlowClient &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  session_.reset();
  backend_.reset();
  graph_ = std::move(other.graph_);
  grammar_ = std::move(other.grammar_);
  node_to_vertex_ = std::move(other.node_to_vertex_);
  return *this;
}

ReachabilityStats ValueFlowClient::solve(SolverBackend backend) {
  if (backend_ && *backend_ != backend) {
    throw std::invalid_argument(
        "Cannot change solver backend after a value-flow session has started");
  }
  if (!session_) {
    session_ = std::make_unique<SolverSession>(graph_, grammar_, backend);
    backend_ = backend;
  }
  return session_->solve();
}

bool ValueFlowClient::hasFlow(std::uint32_t source_node,
                              std::uint32_t target_node) const {
  const auto source_it = node_to_vertex_.find(source_node);
  const auto target_it = node_to_vertex_.find(target_node);
  if (source_it == node_to_vertex_.end() ||
      target_it == node_to_vertex_.end()) {
    return false;
  }
  return session_
             ? session_->contains(source_it->second, target_it->second, "A")
             : graph_.hasEdge(source_it->second, target_it->second, "A");
}

std::vector<std::uint32_t>
ValueFlowClient::reachableFrom(std::uint32_t source_node) const {
  const auto source_it = node_to_vertex_.find(source_node);
  if (source_it == node_to_vertex_.end()) {
    return {};
  }

  std::vector<std::uint32_t> reachable;
  for (const auto &[node_id, vertex_id] : node_to_vertex_) {
    if (session_ ? session_->contains(source_it->second, vertex_id, "A")
                 : graph_.hasEdge(source_it->second, vertex_id, "A")) {
      reachable.push_back(node_id);
    }
  }
  std::sort(reachable.begin(), reachable.end());
  return reachable;
}

} // namespace lotus::cfl::classical
