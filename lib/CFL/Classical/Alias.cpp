#include "CFL/Classical/Alias.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <stdexcept>

namespace lotus::cfl::classical {
namespace {

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

  throw std::logic_error("Invalid alias edge kind value");
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

  throw std::logic_error("Invalid alias edge kind value");
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
          << "Terminal:\n"
          << "  addr addrbar copy copybar store storebar load loadbar vgep "
             "vgepbar";
  for (std::uint32_t attr : attrs) {
    grammar << " gep_" << attr << " gepbar_" << attr;
  }
  grammar << "\n"
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
          << "Terminal:\n"
          << "  addr addrbar copy copybar store storebar load loadbar vgep "
             "vgepbar";
  for (std::uint32_t attr : attrs) {
    grammar << " gep_" << attr << " gepbar_" << attr;
  }
  grammar << "\n"
          << "Productions:\n"
          << "  F -> ( copy M ? ) *;\n"
          << "  Fbar -> ( M ? copybar ) *;\n"
          << "  copy -> vgep;\n"
          << "  copybar -> vgepbar;\n"
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

} // namespace

std::size_t AliasConstraintGraph::addNode(const std::string &name) {
  node_names_.push_back(name);
  return node_names_.size() - 1;
}

void AliasConstraintGraph::addEdge(std::size_t source, std::size_t target,
                                   AliasConstraintEdgeKind kind,
                                   std::optional<std::uint32_t> attribute) {
  if (source >= node_names_.size() || target >= node_names_.size()) {
    throw std::out_of_range("Alias constraint endpoint is out of range");
  }
  if (kind == AliasConstraintEdgeKind::NormalGep && !attribute) {
    throw std::invalid_argument("Normal GEP constraint requires an attribute");
  }
  edges_.push_back({source, target, kind, attribute});
}

LabeledGraph encodePagGraph(const AliasConstraintGraph &graph) {
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

LabeledGraph encodePegGraph(const AliasConstraintGraph &graph) {
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
    return AliasClient(encodePegGraph(graph), buildPegGrammar(graph), mode);
  }
  return AliasClient(encodePagGraph(graph), buildPagGrammar(graph), mode);
}

AliasClient::AliasClient(LabeledGraph graph, Grammar grammar,
                         AliasEncodingMode mode)
    : graph_(std::move(graph)), grammar_(std::move(grammar)), mode_(mode),
      next_synthetic_dereference_(graph_.vertexCount()) {
  initializePegDereferences();
  initializeGepAttributes();
}

AliasClient::AliasClient(AliasClient &&other) noexcept
    : graph_(std::move(other.graph_)), grammar_(std::move(other.grammar_)),
      mode_(other.mode_), peg_dereferences_(std::move(other.peg_dereferences_)),
      next_synthetic_dereference_(other.next_synthetic_dereference_),
      gep_attributes_(std::move(other.gep_attributes_)) {}

AliasClient &AliasClient::operator=(AliasClient &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  session_.reset();
  backend_.reset();
  graph_ = std::move(other.graph_);
  grammar_ = std::move(other.grammar_);
  mode_ = other.mode_;
  peg_dereferences_ = std::move(other.peg_dereferences_);
  next_synthetic_dereference_ = other.next_synthetic_dereference_;
  gep_attributes_ = std::move(other.gep_attributes_);
  return *this;
}

ReachabilityStats AliasClient::solve(SolverBackend backend) {
  if (backend_ && *backend_ != backend) {
    throw std::invalid_argument(
        "Cannot change solver backend after an alias session has started");
  }
  if (!session_) {
    session_ = std::make_unique<SolverSession>(graph_, grammar_, backend);
    backend_ = backend;
  }
  return session_->solve();
}

ReachabilityStats AliasClient::solveToFixedPoint(
    SolverBackend backend,
    const std::function<bool(AliasClient &)> &discover_constraints,
    std::size_t max_rounds) {
  ReachabilityStats aggregate;
  aggregate.solver_rounds = 0;
  for (std::size_t round = 0; round < max_rounds; ++round) {
    const ReachabilityStats current = solve(backend);
    aggregate.graph_nodes = current.graph_nodes;
    aggregate.base_graph_edges = current.base_graph_edges;
    aggregate.grammar_symbols = current.grammar_symbols;
    aggregate.grammar_terminals = current.grammar_terminals;
    aggregate.grammar_nonterminals = current.grammar_nonterminals;
    aggregate.grammar_productions = current.grammar_productions;
    aggregate.grammar_nullable_symbols = current.grammar_nullable_symbols;
    aggregate.grammar_transitive_symbols = current.grammar_transitive_symbols;
    aggregate.classical_iterations += current.classical_iterations;
    aggregate.processed_work_items += current.processed_work_items;
    aggregate.duplicate_edges += current.duplicate_edges;
    aggregate.peak_worklist_size =
        std::max(aggregate.peak_worklist_size, current.peak_worklist_size);
    aggregate.added_edges += current.added_edges;
    aggregate.input_edges = current.input_edges;
    aggregate.relation_edges = current.relation_edges;
    aggregate.start_symbol_edges = current.start_symbol_edges;
    aggregate.solve_time_microseconds += current.solve_time_microseconds;
    aggregate.relation_memory_bytes = current.relation_memory_bytes;
    aggregate.hybrid_forest_roots = current.hybrid_forest_roots;
    aggregate.hybrid_forest_nodes = current.hybrid_forest_nodes;
    aggregate.hybrid_forest_edges = current.hybrid_forest_edges;
    aggregate.hybrid_arc_insertions = current.hybrid_arc_insertions;
    aggregate.hybrid_meld_operations = current.hybrid_meld_operations;
    aggregate.hybrid_duplicate_melds = current.hybrid_duplicate_melds;
    aggregate.hybrid_forest_memory_bytes = current.hybrid_forest_memory_bytes;
    ++aggregate.solver_rounds;
    if (!discover_constraints(*this)) {
      return aggregate;
    }
  }
  throw std::runtime_error(
      "Alias constraint discovery did not stabilize within max_rounds");
}

std::size_t AliasClient::addNode(const std::string &name) {
  if (session_) {
    return session_->addNode(name);
  }
  return graph_.addVertex(name);
}

bool AliasClient::addConstraint(std::size_t source, std::size_t target,
                                AliasConstraintEdgeKind kind,
                                std::optional<std::uint32_t> attribute) {
  if (source >= graph_.vertexCount() || target >= graph_.vertexCount()) {
    throw std::out_of_range("Alias constraint endpoint is out of range");
  }
  if (kind == AliasConstraintEdgeKind::NormalGep && !attribute) {
    throw std::invalid_argument("Normal GEP constraint requires an attribute");
  }
  if (mode_ == AliasEncodingMode::PEG &&
      kind == AliasConstraintEdgeKind::Store) {
    bool changed = false;
    for (std::size_t dereference : ensurePegDereferences(target)) {
      changed =
          addEncodedEdge(source, dereference, "copy", "copybar") || changed;
    }
    return changed;
  }
  if (mode_ == AliasEncodingMode::PEG &&
      kind == AliasConstraintEdgeKind::Load) {
    bool changed = false;
    for (std::size_t dereference : ensurePegDereferences(source)) {
      changed =
          addEncodedEdge(dereference, target, "copy", "copybar") || changed;
    }
    return changed;
  }
  if (kind == AliasConstraintEdgeKind::NormalGep && attribute &&
      !grammar_.isTerminal("gep_" + std::to_string(*attribute))) {
    gep_attributes_.insert(*attribute);
    rebuildGrammar();
  }
  const AliasConstraintEdge edge{source, target, kind, attribute};
  const std::string forward = aliasForwardLabel(edge);
  const std::string reverse = aliasReverseLabel(edge);
  const bool changed = addEncodedEdge(source, target, forward, reverse);
  if (mode_ == AliasEncodingMode::PEG &&
      kind == AliasConstraintEdgeKind::Addr) {
    auto &dereferences = peg_dereferences_[target];
    if (std::find(dereferences.begin(), dereferences.end(), source) ==
        dereferences.end()) {
      dereferences.push_back(source);
    }
  }
  return changed;
}

bool AliasClient::addEncodedEdge(std::size_t source, std::size_t target,
                                 const std::string &forward,
                                 const std::string &reverse) {
  if (!grammar_.isTerminal(forward) || !grammar_.isTerminal(reverse)) {
    throw std::invalid_argument(
        "Constraint attribute was not present when the grammar was built");
  }
  if (!session_) {
    const bool first = graph_.addEdge(source, target, forward);
    const bool second = graph_.addEdge(target, source, reverse);
    return first || second;
  }
  const bool first = session_->addTerminalEdge(source, target, forward);
  const bool second = session_->addTerminalEdge(target, source, reverse);
  return first || second;
}

const std::vector<std::size_t> &
AliasClient::ensurePegDereferences(std::size_t pointer) {
  auto &dereferences = peg_dereferences_[pointer];
  if (!dereferences.empty()) {
    return dereferences;
  }

  const std::string name = "peg_deref_" + std::to_string(pointer) +
                           "_incremental_" +
                           std::to_string(next_synthetic_dereference_++);
  const std::size_t dereference = addNode(name);
  addEncodedEdge(dereference, pointer, "addr", "addrbar");
  dereferences.push_back(dereference);
  return dereferences;
}

void AliasClient::initializePegDereferences() {
  peg_dereferences_.clear();
  if (mode_ != AliasEncodingMode::PEG) {
    return;
  }
  for (const auto &[source, target] : graph_.edgesForLabel("addr")) {
    peg_dereferences_[target].push_back(source);
  }
}

void AliasClient::initializeGepAttributes() {
  gep_attributes_.clear();
  gep_attributes_.insert(0);
  for (const auto &[label, _] : graph_.symbolPairs()) {
    if (label.rfind("gep_", 0) != 0 || label.size() <= 4) {
      continue;
    }
    const std::string value = label.substr(4);
    if (std::all_of(value.begin(), value.end(), [](unsigned char character) {
          return std::isdigit(character) != 0;
        })) {
      gep_attributes_.insert(static_cast<std::uint32_t>(std::stoul(value)));
    }
  }
}

void AliasClient::rebuildGrammar() {
  AliasConstraintGraph shape;
  const std::size_t source = shape.addNode("grammar_source");
  const std::size_t target = shape.addNode("grammar_target");
  for (std::uint32_t attribute : gep_attributes_) {
    shape.addEdge(source, target, AliasConstraintEdgeKind::NormalGep,
                  attribute);
  }
  grammar_ = mode_ == AliasEncodingMode::PEG ? buildPegGrammar(shape)
                                             : buildPagGrammar(shape);
  session_.reset();
  backend_.reset();
}

bool AliasClient::mayAlias(std::size_t lhs, std::size_t rhs) const {
  if (lhs >= graph_.vertexCount() || rhs >= graph_.vertexCount()) {
    return false;
  }
  return session_ ? session_->contains(lhs, rhs, "V")
                  : graph_.hasEdge(lhs, rhs, "V");
}

std::vector<std::size_t> AliasClient::pointsTo(std::size_t ptr) const {
  if (ptr >= graph_.vertexCount()) {
    return {};
  }
  std::set<std::size_t> result;
  std::vector<std::pair<std::size_t, std::size_t>> value_pairs;
  if (session_) {
    const SymbolId value_symbol = grammar_.symbolId("V");
    for (const RelationEdge &edge : session_->relation().edges()) {
      if (edge.symbol == value_symbol) {
        value_pairs.push_back({edge.source, edge.target});
      }
    }
  } else {
    value_pairs = graph_.edgesForLabelCopy("V");
  }

  for (const auto &[source, target] : value_pairs) {
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

} // namespace lotus::cfl::classical
