#include "CFL/Classical/SVFGAdapter.h"

#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>

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

std::uint32_t registeredCallSiteId(const lotus::analysis::SVFG &svfg,
                                   const lotus::analysis::SVFGEdge *edge) {
  const llvm::CallBase *call_site = edge ? edge->getCallSite() : nullptr;
  if (!call_site) {
    return 0;
  }

  const llvm::Function *callee = nullptr;
  if (edge->isCallEdge()) {
    callee = edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
  } else if (edge->isRetEdge()) {
    callee = edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
  }
  return callee ? svfg.getCallSiteId(call_site, callee) : 0;
}

std::uint32_t firstFallbackCallSiteId(const lotus::analysis::SVFG &svfg) {
  std::uint32_t maximum = 0;
  for (const auto &[_, node] : svfg) {
    for (const lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
      maximum = std::max(maximum, registeredCallSiteId(svfg, edge));
    }
  }
  return maximum + 1;
}

std::uint32_t
assignCallSiteId(const lotus::analysis::SVFG &svfg,
                 const lotus::analysis::SVFGEdge *edge,
                 std::unordered_map<std::string, std::uint32_t> &fallback_ids,
                 std::uint32_t &next_fallback_id) {
  const llvm::CallBase *call_site = edge ? edge->getCallSite() : nullptr;
  if (!call_site) {
    return 0;
  }

  const llvm::Function *callee = nullptr;
  if (edge->isCallEdge()) {
    callee = edge->getDstNode() ? edge->getDstNode()->getFunction() : nullptr;
  } else if (edge->isRetEdge()) {
    callee = edge->getSrcNode() ? edge->getSrcNode()->getFunction() : nullptr;
  }

  if (const std::uint32_t id = registeredCallSiteId(svfg, edge); id != 0) {
    return id;
  }

  std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(call_site));
  key.push_back(':');
  key.append(callee ? callee->getName().str() : "unknown");
  auto [it, inserted] = fallback_ids.emplace(key, next_fallback_id);
  if (inserted) {
    ++next_fallback_id;
  }
  return it->second;
}

} // namespace

LabeledGraph encodeSVFG(const lotus::analysis::SVFG &svfg) {
  LabeledGraph encoded;
  for (const auto &[node_id, _] : svfg) {
    encoded.addVertex(nodeName(node_id));
  }

  std::unordered_map<std::string, std::uint32_t> fallback_ids;
  std::uint32_t next_fallback_id = firstFallbackCallSiteId(svfg);

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
        const std::uint32_t id =
            assignCallSiteId(svfg, edge, fallback_ids, next_fallback_id);
        addBidirectionalEdge(encoded, source, target,
                             encodeCallLabel("call", id),
                             encodeCallLabel("callbar", id));
        continue;
      }

      if (edge->isRetEdge()) {
        const std::uint32_t id =
            assignCallSiteId(svfg, edge, fallback_ids, next_fallback_id);
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
  std::unordered_map<std::string, std::uint32_t> fallback_ids;
  std::uint32_t next_fallback_id = firstFallbackCallSiteId(svfg);

  for (const auto &[_, node] : svfg) {
    for (lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || (!edge->isCallEdge() && !edge->isRetEdge())) {
        continue;
      }
      callsite_ids.insert(
          assignCallSiteId(svfg, edge, fallback_ids, next_fallback_id));
    }
  }

  GrammarParseOptions options;
  options.attributes.assign(callsite_ids.begin(), callsite_ids.end());
  if (options.attributes.empty()) {
    options.attributes.push_back(0);
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
