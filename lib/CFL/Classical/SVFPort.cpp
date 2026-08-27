#include "CFL/Classical/SVFPort.h"

#include "IR/SVFG/SVFGBase.h"
#include "IR/SVFG/SVFGEdge.h"
#include "IR/SVFG/SVFGNode.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <stdexcept>

namespace lotus::cfl::classical {
namespace {

std::string nodeName(std::size_t id) { return std::to_string(id); }

std::string aliasForwardLabel(const AliasConstraintEdge &edge) {
  switch (edge.kind) {
  case AliasConstraintEdgeKind::Addr:
    return "addr";
  case AliasConstraintEdgeKind::Copy:
    return "copy";
  case AliasConstraintEdgeKind::Store:
    return "store";
  case AliasConstraintEdgeKind::Load:
    return "load";
  case AliasConstraintEdgeKind::VariantGep:
    return "vgep";
  case AliasConstraintEdgeKind::NormalGep: {
    std::string label = "gep_";
    label +=
        std::to_string(edge.attribute.value_or(static_cast<std::uint32_t>(0)));
    return label;
  }
  }

  throw std::invalid_argument("Unsupported alias edge kind");
}

std::string aliasReverseLabel(const AliasConstraintEdge &edge) {
  switch (edge.kind) {
  case AliasConstraintEdgeKind::Addr:
    return "addrbar";
  case AliasConstraintEdgeKind::Copy:
    return "copybar";
  case AliasConstraintEdgeKind::Store:
    return "storebar";
  case AliasConstraintEdgeKind::Load:
    return "loadbar";
  case AliasConstraintEdgeKind::VariantGep:
    return "vgepbar";
  case AliasConstraintEdgeKind::NormalGep: {
    std::string label = "gepbar_";
    label +=
        std::to_string(edge.attribute.value_or(static_cast<std::uint32_t>(0)));
    return label;
  }
  }

  throw std::invalid_argument("Unsupported alias edge kind");
}

void addBidirectionalEdge(LabeledGraph &graph, std::size_t source,
                          std::size_t target, const std::string &forward,
                          const std::string &reverse) {
  graph.addEdge(source, target, forward);
  graph.addEdge(target, source, reverse);
}

std::set<std::uint32_t>
collectGepAttributes(const AliasConstraintGraph &graph) {
  std::set<std::uint32_t> attrs{0};
  for (const AliasConstraintEdge &edge : graph.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::NormalGep) {
      attrs.insert(edge.attribute.value_or(static_cast<std::uint32_t>(0)));
    }
  }
  return attrs;
}

std::string joinAlternatives(const std::vector<std::string> &alternatives) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < alternatives.size(); ++i) {
    if (i != 0) {
      stream << " | ";
    }
    stream << alternatives[i];
  }
  return stream.str();
}

std::string buildPagGrammarText(const AliasConstraintGraph &graph) {
  const std::set<std::uint32_t> attrs = collectGepAttributes(graph);

  std::vector<std::string> v_alternatives = {"Fbar V F", "addrbar addr",
                                             "gepbarpath V gep_0"};
  std::vector<std::string> memflow_alternatives = {"load store", "Fbar Memflow",
                                                   "F Memflow Fbar"};
  std::vector<std::string> memflowbar_alternatives = {
      "storebar loadbar", "Memflowbar F", "F Memflowbar Fbar"};
  std::vector<std::string> gep_non_zero;
  std::vector<std::string> gepbar_non_zero;

  for (std::uint32_t attr : attrs) {
    std::string gep = "gep_";
    gep += std::to_string(attr);
    std::string gepbar = "gepbar_";
    gepbar += std::to_string(attr);

    std::string gep_v_alternative = gepbar;
    gep_v_alternative += " V ";
    gep_v_alternative += gep;
    v_alternatives.push_back(std::move(gep_v_alternative));

    std::string gep_f_alternative = gepbar;
    gep_f_alternative += " F ";
    gep_f_alternative += gep;
    v_alternatives.push_back(std::move(gep_f_alternative));

    std::string gep_fbar_alternative = gepbar;
    gep_fbar_alternative += " Fbar ";
    gep_fbar_alternative += gep;
    v_alternatives.push_back(std::move(gep_fbar_alternative));

    std::string gep_memflow_alternative = gep;
    gep_memflow_alternative += " Memflow ";
    gep_memflow_alternative += gepbar;
    memflow_alternatives.push_back(std::move(gep_memflow_alternative));

    std::string gepbar_memflow_alternative = gepbar;
    gepbar_memflow_alternative += " Memflow ";
    gepbar_memflow_alternative += gep;
    memflow_alternatives.push_back(std::move(gepbar_memflow_alternative));

    std::string gep_memflowbar_alternative = gep;
    gep_memflowbar_alternative += " Memflowbar ";
    gep_memflowbar_alternative += gepbar;
    memflowbar_alternatives.push_back(std::move(gep_memflowbar_alternative));

    std::string gepbar_memflowbar_alternative = gepbar;
    gepbar_memflowbar_alternative += " Memflowbar ";
    gepbar_memflowbar_alternative += gep;
    memflowbar_alternatives.push_back(std::move(gepbar_memflowbar_alternative));

    if (attr != 0) {
      std::string gep_rule = gep;
      gep_rule += " -> gep_0 F vgep | gep_0 F ";
      gep_rule += gep;
      gep_non_zero.push_back(std::move(gep_rule));

      std::string gepbar_rule = gepbar;
      gepbar_rule += " -> ";
      gepbar_rule += gepbar;
      gepbar_rule += " Fbar gepbar_0 | vgepbar Fbar gepbar_0";
      gepbar_non_zero.push_back(std::move(gepbar_rule));
    }
  }

  std::ostringstream grammar;
  grammar << "Start:\n"
          << "  V\n"
          << "Productions:\n"
          << "  F -> <epsilon> | F copy | addr Memflow | F store V load | "
             "store Memflow load | F F;\n"
          << "  Fbar -> <epsilon> | copybar Fbar | Memflowbar addrbar | "
             "loadbar V storebar Fbar | loadbar Memflowbar storebar;\n"
          << "  V -> " << joinAlternatives(v_alternatives) << ";\n"
          << "  copy -> vgep;\n"
          << "  copybar -> vgepbar;\n"
          << "  gepbarpath -> gepbar_0 gepbar_0 | gepbarpath gepbar_0;\n"
          << "  Memflow -> " << joinAlternatives(memflow_alternatives) << ";\n"
          << "  Memflowbar -> " << joinAlternatives(memflowbar_alternatives)
          << ";\n";

  for (const std::string &rule : gep_non_zero) {
    grammar << "  " << rule << ";\n";
  }
  for (const std::string &rule : gepbar_non_zero) {
    grammar << "  " << rule << ";\n";
  }

  return grammar.str();
}

std::string buildPegGrammarText(const AliasConstraintGraph &graph) {
  const std::set<std::uint32_t> attrs = collectGepAttributes(graph);

  std::vector<std::string> v_alternatives = {"Fbar V F", "M", "<epsilon>",
                                             "ArrayPath V gep_0"};
  std::vector<std::string> memcpy_alternatives = {"addrbar V addr",
                                                  "F Memcpy Fbar"};
  for (std::uint32_t attr : attrs) {
    std::string gep = "gep_";
    gep += std::to_string(attr);
    std::string gepbar = "gepbar_";
    gepbar += std::to_string(attr);

    std::string gep_v_alternative = gepbar;
    gep_v_alternative += " V ";
    gep_v_alternative += gep;
    v_alternatives.push_back(std::move(gep_v_alternative));

    std::string gepbar_memcpy_alternative = gepbar;
    gepbar_memcpy_alternative += " Memcpy ";
    gepbar_memcpy_alternative += gep;
    v_alternatives.push_back(std::move(gepbar_memcpy_alternative));

    std::string gep_memcpy_alternative = gep;
    gep_memcpy_alternative += " Memcpy ";
    gep_memcpy_alternative += gepbar;
    v_alternatives.push_back(gep_memcpy_alternative);
    memcpy_alternatives.push_back(gep_memcpy_alternative);

    std::string gepbar_memcpy_rule = gepbar;
    gepbar_memcpy_rule += " Memcpy ";
    gepbar_memcpy_rule += gep;
    memcpy_alternatives.push_back(std::move(gepbar_memcpy_rule));
  }

  std::ostringstream grammar;
  grammar << "Start:\n"
          << "  V\n"
          << "Productions:\n"
          << "  F -> ( copy M ? ) *;\n"
          << "  Fbar -> ( M ? copybar ) *;\n"
          << "  M -> addr V addrbar;\n"
          << "  V -> " << joinAlternatives(v_alternatives) << ";\n"
          << "  ArrayPath -> gepbar_0 gepbar_0 | ArrayPath gepbar_0;\n"
          << "  Memcpy -> " << joinAlternatives(memcpy_alternatives) << ";\n";
  return grammar.str();
}

std::vector<std::size_t> findAddrSources(const AliasConstraintGraph &graph,
                                         std::size_t target) {
  std::vector<std::size_t> sources;
  for (const AliasConstraintEdge &edge : graph.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::Addr && edge.target == target) {
      sources.push_back(edge.source);
    }
  }
  return sources;
}

std::string encodeCallLabel(const std::string &base, std::uint32_t id) {
  std::string label = base;
  label += '_';
  label += std::to_string(id);
  return label;
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

  if (callee) {
    if (const std::uint32_t id = svfg.getCallSiteId(call_site, callee);
        id != 0) {
      return id;
    }
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

std::size_t AliasConstraintGraph::addNode(const std::string &name) {
  node_names_.push_back(name);
  return node_names_.size() - 1;
}

void AliasConstraintGraph::addEdge(std::size_t source, std::size_t target,
                                   AliasConstraintEdgeKind kind,
                                   std::optional<std::uint32_t> attribute) {
  edges_.push_back({source, target, kind, attribute});
}

LabeledGraph encodeBigraph(const AliasConstraintGraph &graph) {
  LabeledGraph encoded;
  for (const std::string &name : graph.nodeNames()) {
    encoded.addVertex(name);
  }

  for (const AliasConstraintEdge &edge : graph.edges()) {
    addBidirectionalEdge(encoded, edge.source, edge.target,
                         aliasForwardLabel(edge), aliasReverseLabel(edge));
  }

  return encoded;
}

LabeledGraph encodeBiPEGGraph(const AliasConstraintGraph &graph) {
  LabeledGraph encoded;
  for (const std::string &name : graph.nodeNames()) {
    encoded.addVertex(name);
  }

  std::size_t synthetic_id = graph.nodeNames().size();
  auto makeSyntheticDeref = [&](std::size_t original_node,
                                const std::string &tag) -> std::size_t {
    std::string label = "peg_deref_";
    label += std::to_string(original_node);
    label += '_';
    label += tag;
    label += '_';
    label += std::to_string(synthetic_id++);
    return encoded.addVertex(label);
  };

  for (const AliasConstraintEdge &edge : graph.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::Store) {
      std::vector<std::size_t> deref_nodes =
          findAddrSources(graph, edge.target);
      if (deref_nodes.empty()) {
        deref_nodes.push_back(makeSyntheticDeref(edge.target, "store"));
        addBidirectionalEdge(encoded, deref_nodes.front(), edge.target, "addr",
                             "addrbar");
      }
      for (std::size_t deref : deref_nodes) {
        addBidirectionalEdge(encoded, edge.source, deref, "copy", "copybar");
      }
      continue;
    }

    if (edge.kind == AliasConstraintEdgeKind::Load) {
      std::vector<std::size_t> deref_nodes =
          findAddrSources(graph, edge.source);
      if (deref_nodes.empty()) {
        deref_nodes.push_back(makeSyntheticDeref(edge.source, "load"));
        addBidirectionalEdge(encoded, deref_nodes.front(), edge.source, "addr",
                             "addrbar");
      }
      for (std::size_t deref : deref_nodes) {
        addBidirectionalEdge(encoded, deref, edge.target, "copy", "copybar");
      }
      continue;
    }

    if (edge.kind == AliasConstraintEdgeKind::VariantGep) {
      addBidirectionalEdge(encoded, edge.source, edge.target, "copy",
                           "copybar");
      continue;
    }

    addBidirectionalEdge(encoded, edge.source, edge.target,
                         aliasForwardLabel(edge), aliasReverseLabel(edge));
  }

  return encoded;
}

Grammar buildPagGrammar(const AliasConstraintGraph &graph) {
  return Grammar::parseFromText(buildPagGrammarText(graph));
}

Grammar buildPegGrammar(const AliasConstraintGraph &graph) {
  return Grammar::parseFromText(buildPegGrammarText(graph));
}

AliasClient AliasClient::fromConstraintGraph(const AliasConstraintGraph &graph,
                                             AliasEncodingMode mode) {
  if (mode == AliasEncodingMode::PEG) {
    return AliasClient(encodeBiPEGGraph(graph), buildPegGrammar(graph));
  }
  return AliasClient(encodeBigraph(graph), buildPagGrammar(graph));
}

ReachabilityStats AliasClient::solve() {
  const CFLSolver solver;
  return solver.solve(graph_, grammar_);
}

bool AliasClient::mayAlias(std::size_t lhs, std::size_t rhs) const {
  return lhs < graph_.vertexCount() && rhs < graph_.vertexCount() &&
         graph_.hasEdge(lhs, rhs, "V");
}

std::vector<std::size_t> AliasClient::pointsTo(std::size_t ptr) const {
  std::set<std::size_t> result;
  for (const auto &[source, target] : graph_.edgesForLabel("V")) {
    if (source != ptr) {
      continue;
    }

    bool added_precise_target = false;
    for (std::size_t pred : graph_.predecessorsForLabel(target, "addr")) {
      result.insert(pred);
      added_precise_target = true;
    }

    for (const auto &[label, _] : graph_.symbolPairs()) {
      if (label.rfind("gep_", 0) != 0) {
        continue;
      }
      for (std::size_t pred : graph_.predecessorsForLabel(target, label)) {
        result.insert(pred);
        added_precise_target = true;
      }
    }

    if (!added_precise_target) {
      result.insert(target);
    }
  }

  return {result.begin(), result.end()};
}

LabeledGraph encodeSVFG(const lotus::analysis::SVFG &svfg) {
  LabeledGraph encoded;
  for (const auto &[node_id, _] : svfg) {
    encoded.addVertex(nodeName(node_id));
  }

  std::unordered_map<std::string, std::uint32_t> fallback_ids;
  std::uint32_t next_fallback_id = 1;

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
  std::uint32_t next_fallback_id = 1;

  for (const auto &[_, node] : svfg) {
    for (lotus::analysis::SVFGEdge *edge : node->getOutEdges()) {
      if (!edge || (!edge->isCallEdge() && !edge->isRetEdge())) {
        continue;
      }
      callsite_ids.insert(
          assignCallSiteId(svfg, edge, fallback_ids, next_fallback_id));
    }
  }

  std::vector<std::string> a_alternatives = {"A A", "a", "<epsilon>"};
  std::vector<std::string> abar_alternatives = {"Abar Abar", "abar",
                                                "<epsilon>"};

  for (std::uint32_t id : callsite_ids) {
    std::string a_alternative = encodeCallLabel("call", id);
    a_alternative += " A ";
    a_alternative += encodeCallLabel("ret", id);
    a_alternatives.push_back(std::move(a_alternative));

    std::string abar_alternative = encodeCallLabel("retbar", id);
    abar_alternative += " Abar ";
    abar_alternative += encodeCallLabel("callbar", id);
    abar_alternatives.push_back(std::move(abar_alternative));
  }

  std::ostringstream grammar;
  grammar << "Start:\n"
          << "  A\n"
          << "Productions:\n"
          << "  A -> " << joinAlternatives(a_alternatives) << ";\n"
          << "  Abar -> " << joinAlternatives(abar_alternatives) << ";\n";
  return Grammar::parseFromText(grammar.str());
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

ReachabilityStats ValueFlowClient::solve() {
  const CFLSolver solver;
  return solver.solve(graph_, grammar_);
}

bool ValueFlowClient::hasFlow(std::uint32_t source_node,
                              std::uint32_t target_node) const {
  const auto source_it = node_to_vertex_.find(source_node);
  const auto target_it = node_to_vertex_.find(target_node);
  if (source_it == node_to_vertex_.end() ||
      target_it == node_to_vertex_.end()) {
    return false;
  }
  return graph_.hasEdge(source_it->second, target_it->second, "A");
}

std::vector<std::uint32_t>
ValueFlowClient::reachableFrom(std::uint32_t source_node) const {
  const auto source_it = node_to_vertex_.find(source_node);
  if (source_it == node_to_vertex_.end()) {
    return {};
  }

  std::vector<std::uint32_t> reachable;
  for (const auto &[node_id, vertex_id] : node_to_vertex_) {
    if (graph_.hasEdge(source_it->second, vertex_id, "A")) {
      reachable.push_back(node_id);
    }
  }
  std::sort(reachable.begin(), reachable.end());
  return reachable;
}

} // namespace lotus::cfl::classical
