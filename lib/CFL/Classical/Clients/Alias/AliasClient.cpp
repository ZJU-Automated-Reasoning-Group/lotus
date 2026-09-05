#include "CFL/Classical/Clients/Alias/AliasClient.h"

#include "CFL/Classical/Core/Validation.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>

#include <llvm/ADT/SparseBitVector.h>

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
  case AliasConstraintEdgeKind::MemoryTransfer:
    throw std::logic_error("Memory transfers have no PAG terminal");
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
  case AliasConstraintEdgeKind::MemoryTransfer:
    throw std::logic_error("Memory transfers have no reverse PAG terminal");
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
  result.relation_edges =
      source.reachability_pairs + source.value_or_flow_pairs;
  result.start_symbol_edges = source.value_or_flow_pairs;
  result.classical_iterations = source.reachability_checks;
  result.processed_work_items = source.processed_items;
  result.duplicate_edges = source.duplicate_items;
  result.added_edges = result.relation_edges;
  result.specialized_reachability_pairs = source.reachability_pairs;
  result.specialized_matched_pairs = source.matched_pairs;
  result.specialized_critical_edges = source.critical_edges;
  result.fully_ordered_cycle_simplifications = source.cycle_simplifications;
  return result;
}

std::string buildPagGrammarText(const AliasConstraintGraph &graph) {
  const std::set<std::uint32_t> attrs = collectGepAttributes(graph);

  std::vector<std::string> v_alternatives = {
      "Fbar V F", "addrbar addr", "gepbarpath V gep_0", "gepbar_0 V geppath"};
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
    const std::string grammar_gep =
        attr == 0 ? gep : "Gep_" + std::to_string(attr);
    const std::string grammar_gepbar =
        attr == 0 ? gepbar : "Gepbar_" + std::to_string(attr);

    std::string gep_v_alternative = grammar_gepbar;
    gep_v_alternative += " V ";
    gep_v_alternative += grammar_gep;
    v_alternatives.push_back(std::move(gep_v_alternative));

    // A variant GEP denotes an unknown offset and must conservatively overlap
    // every fixed (or unknown) field rooted at the same abstract object.
    v_alternatives.push_back("vgepbar V " + grammar_gep);
    v_alternatives.push_back(grammar_gepbar + " V vgep");

    std::string gep_f_alternative = grammar_gepbar;
    gep_f_alternative += " F ";
    gep_f_alternative += grammar_gep;
    v_alternatives.push_back(std::move(gep_f_alternative));

    std::string gep_fbar_alternative = grammar_gepbar;
    gep_fbar_alternative += " Fbar ";
    gep_fbar_alternative += grammar_gep;
    v_alternatives.push_back(std::move(gep_fbar_alternative));

    std::string gep_memflow_alternative = grammar_gep;
    gep_memflow_alternative += " Memflow ";
    gep_memflow_alternative += grammar_gepbar;
    memflow_alternatives.push_back(std::move(gep_memflow_alternative));

    std::string gepbar_memflow_alternative = grammar_gepbar;
    gepbar_memflow_alternative += " Memflow ";
    gepbar_memflow_alternative += grammar_gep;
    memflow_alternatives.push_back(std::move(gepbar_memflow_alternative));

    std::string gep_memflowbar_alternative = grammar_gep;
    gep_memflowbar_alternative += " Memflowbar ";
    gep_memflowbar_alternative += grammar_gepbar;
    memflowbar_alternatives.push_back(std::move(gep_memflowbar_alternative));

    std::string gepbar_memflowbar_alternative = grammar_gepbar;
    gepbar_memflowbar_alternative += " Memflowbar ";
    gepbar_memflowbar_alternative += grammar_gep;
    memflowbar_alternatives.push_back(std::move(gepbar_memflowbar_alternative));

    if (attr != 0) {
      std::string gep_rule = grammar_gep;
      gep_rule += " -> ";
      gep_rule += gep;
      gep_rule += " | gep_0 F vgep | gep_0 F ";
      gep_rule += grammar_gep;
      gep_non_zero.push_back(std::move(gep_rule));

      std::string gepbar_rule = grammar_gepbar;
      gepbar_rule += " -> ";
      gepbar_rule += gepbar;
      gepbar_rule += " | ";
      gepbar_rule += grammar_gepbar;
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
          << "  F -> <epsilon> | F Copy | addr Memflow | F store V load | "
             "store Memflow load | F F;\n"
          << "  Fbar -> <epsilon> | Copybar Fbar | Memflowbar addrbar | "
             "loadbar V storebar Fbar | loadbar Memflowbar storebar;\n"
          << "  V -> " << joinAlternatives(v_alternatives) << ";\n"
          << "  Copy -> copy | vgep;\n"
          << "  Copybar -> copybar | vgepbar;\n"
          << "  gepbarpath -> gepbar_0 gepbar_0 | gepbarpath gepbar_0;\n"
          << "  geppath -> gep_0 gep_0 | geppath gep_0;\n"
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
                                             "ArrayPath V gep_0",
                                             "gepbar_0 V ArrayPathForward"};
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
    v_alternatives.push_back("vgepbar V " + gep);
    v_alternatives.push_back(gepbar + " V vgep");

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
          << "  F -> ( Copy M ? ) *;\n"
          << "  Fbar -> ( M ? Copybar ) *;\n"
          << "  Copy -> copy | vgep;\n"
          << "  Copybar -> copybar | vgepbar;\n"
          << "  M -> addr V addrbar;\n"
          << "  V -> " << joinAlternatives(v_alternatives) << ";\n"
          << "  ArrayPath -> gepbar_0 gepbar_0 | ArrayPath gepbar_0;\n"
          << "  ArrayPathForward -> gep_0 gep_0 | ArrayPathForward gep_0;\n"
          << "  Memcpy -> " << joinAlternatives(memcpy_alternatives) << ";\n";
  return grammar.str();
}

} // namespace

struct AliasClient::State {
  State(LabeledGraph input_graph, Grammar input_grammar)
      : graph(std::move(input_graph)), grammar(std::move(input_grammar)) {}

  LabeledGraph graph;
  Grammar grammar;
};

std::size_t AliasConstraintGraph::addNode(const std::string &name) {
  node_names_.push_back(name);
  return node_names_.size() - 1;
}

void AliasConstraintGraph::addEdge(std::size_t source, std::size_t target,
                                   AliasConstraintEdgeKind kind,
                                   std::optional<std::uint32_t> attribute,
                                   std::optional<std::uint32_t> modulus) {
  if (source >= node_names_.size() || target >= node_names_.size()) {
    throw std::out_of_range("Alias constraint endpoint is out of range");
  }
  if (kind == AliasConstraintEdgeKind::NormalGep && !attribute) {
    throw std::invalid_argument("Normal GEP constraint requires an attribute");
  }
  if (modulus && *modulus == 0) {
    throw std::invalid_argument("Variant GEP modulus must be non-zero");
  }
  edges_.push_back({source, target, kind, attribute, modulus});
}

LabeledGraph encodePagGraph(const AliasConstraintGraph &graph) {
  LabeledGraph encoded;
  for (const std::string &name : graph.nodeNames()) {
    encoded.addVertex(name);
  }

  for (const AliasConstraintEdge &edge : graph.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::MemoryTransfer) {
      continue;
    }
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

  std::unordered_map<std::size_t, std::vector<std::size_t>> dereference_nodes;
  for (const AliasConstraintEdge &edge : graph.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::Addr) {
      dereference_nodes[edge.target].push_back(edge.source);
    }
  }
  auto dereferences =
      [&](std::size_t pointer) -> const std::vector<std::size_t> & {
    auto &result = dereference_nodes[pointer];
    if (result.empty()) {
      const std::size_t dereference = encoded.addVertex(
          "peg_deref_" + std::to_string(pointer) + "_synthetic");
      addBidirectionalEdge(encoded, dereference, pointer, "addr", "addrbar");
      result.push_back(dereference);
    }
    return result;
  };

  for (const AliasConstraintEdge &edge : graph.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::MemoryTransfer) {
      continue;
    }
    if (edge.kind == AliasConstraintEdgeKind::Store) {
      for (std::size_t deref : dereferences(edge.target)) {
        addBidirectionalEdge(encoded, edge.source, deref, "copy", "copybar");
      }
      continue;
    }

    if (edge.kind == AliasConstraintEdgeKind::Load) {
      for (std::size_t deref : dereferences(edge.source)) {
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
    return AliasClient(encodePegGraph(graph), buildPegGrammar(graph), graph,
                       mode);
  }
  return AliasClient(encodePagGraph(graph), buildPagGrammar(graph), graph,
                     mode);
}

AliasClient::AliasClient(LabeledGraph graph, Grammar grammar,
                         AliasConstraintGraph constraints,
                         AliasEncodingMode mode)
    : state_(std::make_unique<State>(std::move(graph), std::move(grammar))),
      constraints_(std::move(constraints)), mode_(mode),
      next_synthetic_dereference_(state_->graph.vertexCount()) {
  while (constraints_.nodeNames().size() < state_->graph.vertexCount()) {
    constraints_.addNode("peg_internal_" +
                         std::to_string(constraints_.nodeNames().size()));
  }
  initializePegDereferences();
  initializeGepAttributes();
}

AliasClient::~AliasClient() = default;

AliasClient::AliasClient(AliasClient &&other) noexcept
    : state_(std::move(other.state_)),
      constraints_(std::move(other.constraints_)), mode_(other.mode_),
      peg_dereferences_(std::move(other.peg_dereferences_)),
      next_synthetic_dereference_(other.next_synthetic_dereference_),
      gep_attributes_(std::move(other.gep_attributes_)),
      grammar_dirty_(other.grammar_dirty_),
      points_to_(std::move(other.points_to_)),
      location_nodes_(std::move(other.location_nodes_)),
      pointee_bases_(std::move(other.pointee_bases_)),
      points_to_valid_(other.points_to_valid_),
      address_objects_(std::move(other.address_objects_)),
      address_object_sources_(std::move(other.address_object_sources_)),
      address_objects_valid_(other.address_objects_valid_),
      session_(std::move(other.session_)), backend_(std::move(other.backend_)),
      pocr_engine_(std::move(other.pocr_engine_)),
      focr_engine_(std::move(other.focr_engine_)),
      specialized_graph_(std::move(other.specialized_graph_)),
      specialized_backend_(std::move(other.specialized_backend_)),
      specialized_focr_cycles_(other.specialized_focr_cycles_) {}

AliasClient &AliasClient::operator=(AliasClient &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  session_.reset();
  state_ = std::move(other.state_);
  constraints_ = std::move(other.constraints_);
  mode_ = other.mode_;
  peg_dereferences_ = std::move(other.peg_dereferences_);
  next_synthetic_dereference_ = other.next_synthetic_dereference_;
  gep_attributes_ = std::move(other.gep_attributes_);
  grammar_dirty_ = other.grammar_dirty_;
  points_to_ = std::move(other.points_to_);
  location_nodes_ = std::move(other.location_nodes_);
  pointee_bases_ = std::move(other.pointee_bases_);
  points_to_valid_ = other.points_to_valid_;
  address_objects_ = std::move(other.address_objects_);
  address_object_sources_ = std::move(other.address_object_sources_);
  address_objects_valid_ = other.address_objects_valid_;
  session_ = std::move(other.session_);
  backend_ = std::move(other.backend_);
  pocr_engine_ = std::move(other.pocr_engine_);
  focr_engine_ = std::move(other.focr_engine_);
  specialized_graph_ = std::move(other.specialized_graph_);
  specialized_backend_ = std::move(other.specialized_backend_);
  specialized_focr_cycles_ = other.specialized_focr_cycles_;
  return *this;
}

const LabeledGraph &AliasClient::graph() const { return state_->graph; }

const Grammar &AliasClient::grammar() const {
  if (grammar_dirty_) {
    const_cast<AliasClient *>(this)->rebuildGrammar();
  }
  return state_->grammar;
}

ReachabilityStats AliasClient::solve(SolverBackend backend) {
  address_objects_valid_ = false;
  if (grammar_dirty_) {
    rebuildGrammar();
  }
  if (specialized_backend_) {
    throw std::invalid_argument(
        "Cannot switch from a specialized alias engine to SolverSession");
  }
  if (backend_ && *backend_ != backend) {
    throw std::invalid_argument(
        "Cannot change solver backend after an alias session has started");
  }
  if (!session_) {
    session_ = std::make_unique<SolverSession>(state_->graph, state_->grammar,
                                               backend);
    backend_ = backend;
  }
  return session_->solve();
}

ReachabilityStats
AliasClient::solveSpecialized(engines::SpecializedPocrBackend backend,
                              bool simplify_focr_cycles) {
  const auto start = std::chrono::steady_clock::now();
  address_objects_valid_ = false;
  simplify_focr_cycles =
      backend == engines::SpecializedPocrBackend::Focr && simplify_focr_cycles;
  if (session_) {
    throw std::invalid_argument(
        "Cannot switch from SolverSession to a specialized alias engine");
  }
  if (specialized_backend_ && *specialized_backend_ != backend) {
    throw std::invalid_argument(
        "Cannot change specialized alias engine after solving has started");
  }
  if (specialized_backend_ &&
      specialized_focr_cycles_ != simplify_focr_cycles) {
    throw std::invalid_argument(
        "Cannot change FOCR cycle simplification after solving has started");
  }
  specialized_backend_ = backend;
  specialized_focr_cycles_ = simplify_focr_cycles;
  if (!specialized_graph_) {
    specialized_graph_ =
        std::make_unique<LabeledGraph>(buildSpecializedAliasGraph());
  }
  if (backend == engines::SpecializedPocrBackend::Pocr) {
    if (!pocr_engine_) {
      pocr_engine_ =
          std::make_unique<engines::PocrAliasEngine>(*specialized_graph_);
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
    focr_engine_ = std::make_unique<engines::FocrAliasEngine>(
        *specialized_graph_, simplify_focr_cycles);
  }
  ReachabilityStats result =
      specializedStats(focr_engine_->solve(), state_->grammar);
  result.solve_time_microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count();
  return result;
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
    aggregate.count_symbol_edges = current.count_symbol_edges;
    aggregate.solve_time_microseconds += current.solve_time_microseconds;
    aggregate.relation_payload_bytes_estimate =
        current.relation_payload_bytes_estimate;
    aggregate.candidate_relation_edges = current.candidate_relation_edges;
    aggregate.transitive_closure_instances =
        current.transitive_closure_instances;
    aggregate.transitive_relation_edges = current.transitive_relation_edges;
    aggregate.transitive_arc_insertions += current.transitive_arc_insertions;
    aggregate.transitive_propagated_pairs +=
        current.transitive_propagated_pairs;
    aggregate.transitive_duplicate_pairs += current.transitive_duplicate_pairs;
    aggregate.transitive_payload_bytes_estimate =
        current.transitive_payload_bytes_estimate;
    aggregate.pocr_tree_roots = current.pocr_tree_roots;
    aggregate.pocr_tree_nodes = current.pocr_tree_nodes;
    aggregate.pocr_tree_edges = current.pocr_tree_edges;
    aggregate.pocr_traversal_steps += current.pocr_traversal_steps;
    aggregate.pocr_tree_join_visits += current.pocr_tree_join_visits;
    aggregate.fully_ordered_critical_edges =
        current.fully_ordered_critical_edges;
    aggregate.fully_ordered_reachability_checks +=
        current.fully_ordered_reachability_checks;
    aggregate.fully_ordered_tree_join_visits +=
        current.fully_ordered_tree_join_visits;
    aggregate.fully_ordered_critical_edge_insertions +=
        current.fully_ordered_critical_edge_insertions;
    aggregate.fully_ordered_critical_edge_removals +=
        current.fully_ordered_critical_edge_removals;
    aggregate.fully_ordered_cycle_simplifications +=
        current.fully_ordered_cycle_simplifications;
    aggregate.graspan_epochs += current.graspan_epochs;
    ++aggregate.solver_rounds;
    if (!discover_constraints(*this)) {
      return aggregate;
    }
  }
  throw std::runtime_error(
      "Alias constraint discovery did not stabilize within max_rounds");
}

ReachabilityStats AliasClient::solveToFixedPoint(
    engines::SpecializedPocrBackend backend,
    const std::function<bool(AliasClient &)> &discover_constraints,
    std::size_t max_rounds, bool simplify_focr_cycles) {
  ReachabilityStats aggregate;
  aggregate.solver_rounds = 0;
  for (std::size_t round = 0; round < max_rounds; ++round) {
    const ReachabilityStats current =
        solveSpecialized(backend, simplify_focr_cycles);
    const std::size_t prior_rounds = aggregate.solver_rounds;
    const std::uint64_t prior_iterations = aggregate.classical_iterations;
    const std::size_t prior_processed = aggregate.processed_work_items;
    const std::size_t prior_duplicates = aggregate.duplicate_edges;
    const std::size_t prior_added = aggregate.added_edges;
    aggregate = current;
    aggregate.solver_rounds = prior_rounds + 1;
    aggregate.classical_iterations += prior_iterations;
    aggregate.processed_work_items += prior_processed;
    aggregate.duplicate_edges += prior_duplicates;
    aggregate.added_edges += prior_added;
    if (!discover_constraints(*this)) {
      return aggregate;
    }
  }
  throw std::runtime_error(
      "Specialized alias constraint discovery did not stabilize within "
      "max_rounds");
}

std::size_t AliasClient::addNode(const std::string &name) {
  return addInternalNode(name);
}

std::size_t AliasClient::addInternalNode(const std::string &name) {
  points_to_valid_ = false;
  address_objects_valid_ = false;
  const std::size_t semantic_node = constraints_.addNode(name);
  if (session_) {
    const std::size_t graph_node = session_->addNode(name);
    if (semantic_node != graph_node) {
      throw std::logic_error("Alias semantic and encoded node IDs diverged");
    }
    return graph_node;
  }
  const std::size_t node = state_->graph.addVertex(name);
  if (semantic_node != node) {
    throw std::logic_error("Alias semantic and encoded node IDs diverged");
  }
  invalidateSpecializedEngines();
  return node;
}

bool AliasClient::addConstraint(std::size_t source, std::size_t target,
                                AliasConstraintEdgeKind kind,
                                std::optional<std::uint32_t> attribute,
                                std::optional<std::uint32_t> modulus) {
  if (source >= state_->graph.vertexCount() ||
      target >= state_->graph.vertexCount()) {
    throw std::out_of_range("Alias constraint endpoint is out of range");
  }
  if (kind == AliasConstraintEdgeKind::NormalGep && !attribute) {
    throw std::invalid_argument("Normal GEP constraint requires an attribute");
  }
  if (modulus && *modulus == 0) {
    throw std::invalid_argument("Variant GEP modulus must be non-zero");
  }
  bool semantic_change = true;
  for (const AliasConstraintEdge &edge : constraints_.edges()) {
    if (edge.source == source && edge.target == target && edge.kind == kind &&
        edge.attribute == attribute && edge.modulus == modulus) {
      semantic_change = false;
      break;
    }
  }
  if (semantic_change) {
    constraints_.addEdge(source, target, kind, attribute, modulus);
    points_to_valid_ = false;
    address_objects_valid_ = false;
  }
  if (kind == AliasConstraintEdgeKind::MemoryTransfer) {
    if (semantic_change) {
      invalidateSpecializedEngines();
    }
    return semantic_change;
  }
  if (mode_ == AliasEncodingMode::PEG &&
      kind == AliasConstraintEdgeKind::Store) {
    bool changed = false;
    for (std::size_t dereference : ensurePegDereferences(target)) {
      changed =
          addEncodedEdge(source, dereference, "copy", "copybar") || changed;
    }
    return changed || semantic_change;
  }
  if (mode_ == AliasEncodingMode::PEG &&
      kind == AliasConstraintEdgeKind::Load) {
    bool changed = false;
    for (std::size_t dereference : ensurePegDereferences(source)) {
      changed =
          addEncodedEdge(dereference, target, "copy", "copybar") || changed;
    }
    return changed || semantic_change;
  }
  if (kind == AliasConstraintEdgeKind::NormalGep && attribute &&
      !state_->grammar.isTerminal("gep_" + std::to_string(*attribute))) {
    registerGepAttributes({*attribute});
  }
  const AliasConstraintEdge edge{source, target, kind, attribute, modulus};
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
  return changed || semantic_change;
}

bool AliasClient::addMemoryTransfer(std::size_t source, std::size_t target,
                                    std::optional<std::uint32_t> size) {
  return addConstraint(source, target, AliasConstraintEdgeKind::MemoryTransfer,
                       size);
}

bool AliasClient::registerGepAttributes(
    const std::set<std::uint32_t> &attributes) {
  bool changed = false;
  for (std::uint32_t attribute : attributes) {
    changed = gep_attributes_.insert(attribute).second || changed;
  }
  if (changed) {
    if (specialized_backend_) {
      grammar_dirty_ = true;
    } else {
      rebuildGrammar();
    }
  }
  return changed;
}

bool AliasClient::addEncodedEdge(std::size_t source, std::size_t target,
                                 const std::string &forward,
                                 const std::string &reverse) {
  const bool deferred_specialized_gep =
      specialized_backend_ && !session_ && grammar_dirty_ &&
      forward.rfind("gep_", 0) == 0 && reverse.rfind("gepbar_", 0) == 0;
  if ((!state_->grammar.isTerminal(forward) ||
       !state_->grammar.isTerminal(reverse)) &&
      !deferred_specialized_gep) {
    throw std::invalid_argument(
        "Constraint attribute was not present when the grammar was built");
  }
  if (!session_) {
    const bool first = state_->graph.addEdge(source, target, forward);
    const bool second = state_->graph.addEdge(target, source, reverse);
    if (first || second) {
      invalidateSpecializedEngines();
    }
    return first || second;
  }
  const bool first = session_->addTerminalEdge(source, target, forward);
  const bool second = session_->addTerminalEdge(target, source, reverse);
  return first || second;
}

void AliasClient::invalidateSpecializedEngines() {
  pocr_engine_.reset();
  focr_engine_.reset();
  specialized_graph_.reset();
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
  const std::size_t dereference = addInternalNode(name);
  addEncodedEdge(dereference, pointer, "addr", "addrbar");
  dereferences.push_back(dereference);
  return dereferences;
}

void AliasClient::initializePegDereferences() {
  peg_dereferences_.clear();
  if (mode_ != AliasEncodingMode::PEG) {
    return;
  }
  for (const auto &[source, target] : state_->graph.edgesForLabel("addr")) {
    peg_dereferences_[target].push_back(source);
  }
}

void AliasClient::initializeGepAttributes() {
  gep_attributes_.clear();
  gep_attributes_.insert(0);
  for (const auto &[label, _] : state_->graph.symbolPairs()) {
    if (label.rfind("gep_", 0) != 0 || label.size() <= 4) {
      continue;
    }
    if (const auto attribute =
            parseAttributeValue(std::string_view(label).substr(4))) {
      gep_attributes_.insert(*attribute);
    }
  }
}

void AliasClient::rebuildGrammar() {
  struct NamedFact {
    std::string symbol;
    std::size_t source;
    std::size_t target;
  };
  std::vector<NamedFact> preserved_facts;
  if (session_) {
    for (const RelationEdge &edge : session_->relation().edges()) {
      const std::string &symbol = state_->grammar.symbolName(edge.symbol);
      if (!state_->grammar.isGeneratedNonterminal(symbol)) {
        preserved_facts.push_back({symbol, edge.source, edge.target});
      }
    }
  }

  AliasConstraintGraph shape;
  const std::size_t source = shape.addNode("grammar_source");
  const std::size_t target = shape.addNode("grammar_target");
  for (std::uint32_t attribute : gep_attributes_) {
    shape.addEdge(source, target, AliasConstraintEdgeKind::NormalGep,
                  attribute);
  }
  Grammar extended_grammar = mode_ == AliasEncodingMode::PEG
                                 ? buildPegGrammar(shape)
                                 : buildPagGrammar(shape);
  const std::optional<SolverBackend> active_backend = backend_;
  session_.reset();
  state_->grammar = std::move(extended_grammar);
  grammar_dirty_ = false;
  if (active_backend) {
    session_ = std::make_unique<SolverSession>(state_->graph, state_->grammar,
                                               *active_backend);
    for (const NamedFact &fact : preserved_facts) {
      if (state_->grammar.hasSymbol(fact.symbol)) {
        session_->addKnownRelationEdge(fact.source, fact.target, fact.symbol);
      }
    }
  }
}

bool AliasClient::mayAlias(std::size_t lhs, std::size_t rhs) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  if (lhs >= state_->graph.vertexCount() ||
      rhs >= state_->graph.vertexCount()) {
    return false;
  }
  if (!points_to_valid_) {
    rebuildPointsTo();
  }
  if (lhs < points_to_.size() && rhs < points_to_.size() &&
      (!points_to_[lhs].empty() || !points_to_[rhs].empty())) {
    return pointsToOverlap(lhs, rhs);
  }
  return mayValueAlias(lhs, rhs);
}

bool AliasClient::mayValueAlias(std::size_t lhs, std::size_t rhs) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  if (lhs >= state_->graph.vertexCount() ||
      rhs >= state_->graph.vertexCount()) {
    return false;
  }
  if (specialized_backend_) {
    if (*specialized_backend_ == engines::SpecializedPocrBackend::Pocr) {
      if (!pocr_engine_) {
        throw std::logic_error(
            "Specialized alias graph changed; solve again before querying");
      }
      return pocr_engine_->mayAlias(lhs, rhs);
    }
    if (!focr_engine_) {
      throw std::logic_error(
          "Specialized alias graph changed; solve again before querying");
    }
    return focr_engine_->mayAlias(lhs, rhs);
  }
  if (!session_) {
    throw std::logic_error("solve() has not been called");
  }
  return session_->contains(lhs, rhs, "V");
}

std::vector<std::size_t>
AliasClient::addressTakenObjects(std::size_t ptr) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  if (ptr >= state_->graph.vertexCount()) {
    return {};
  }
  indexAddressTakenObjects({ptr});
  if (const auto it = address_objects_.find(ptr);
      it != address_objects_.end()) {
    return it->second;
  }
  return {};
}

std::unordered_map<std::size_t, std::vector<std::size_t>>
AliasClient::addressTakenObjects(
    const std::vector<std::size_t> &pointers) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  indexAddressTakenObjects(pointers);
  std::unordered_map<std::size_t, std::vector<std::size_t>> result;
  for (std::size_t pointer : pointers) {
    if (const auto it = address_objects_.find(pointer);
        it != address_objects_.end()) {
      result.emplace(pointer, it->second);
    }
  }
  return result;
}

std::unordered_map<std::size_t, std::vector<std::size_t>>
AliasClient::matchingAddressTakenObjects(
    const std::vector<std::size_t> &pointers,
    const std::vector<std::pair<std::size_t, std::size_t>>
        &object_pointer_candidates) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  std::unordered_map<std::size_t, std::vector<std::size_t>> result;
  for (std::size_t pointer : pointers) {
    if (pointer >= state_->graph.vertexCount()) {
      continue;
    }
    auto &objects = result[pointer];
    for (const auto &[object, address_pointer] : object_pointer_candidates) {
      if (address_pointer < state_->graph.vertexCount() &&
          mayValueAlias(pointer, address_pointer)) {
        objects.push_back(object);
      }
    }
  }
  return result;
}

void AliasClient::indexAddressTakenObjects(
    const std::vector<std::size_t> &pointers) const {
  if (!address_objects_valid_) {
    address_objects_.clear();
    address_object_sources_.clear();
    address_objects_valid_ = true;
  }
  std::unordered_set<std::size_t> pending;
  for (std::size_t pointer : pointers) {
    if (pointer < state_->graph.vertexCount() &&
        address_object_sources_.insert(pointer).second) {
      pending.insert(pointer);
    }
  }
  if (pending.empty()) {
    return;
  }

  std::unordered_map<std::size_t, std::vector<std::size_t>> objects_by_pointer;
  for (const auto &[object, pointer] : state_->graph.edgesForLabel("addr")) {
    objects_by_pointer[pointer].push_back(object);
  }
  std::unordered_map<std::size_t, std::set<std::size_t>> unique_objects;
  auto project_pair = [&](std::size_t source, std::size_t target) {
    if (pending.count(source) == 0 || target >= state_->graph.vertexCount()) {
      return;
    }
    const auto objects = objects_by_pointer.find(target);
    if (objects == objects_by_pointer.end()) {
      return;
    }
    unique_objects[source].insert(objects->second.begin(),
                                  objects->second.end());
  };

  if (specialized_backend_) {
    const std::vector<std::pair<NodeId, NodeId>> pairs =
        *specialized_backend_ == engines::SpecializedPocrBackend::Pocr
            ? pocr_engine_->valuePairs()
            : focr_engine_->valuePairs();
    for (const auto &[source, target] : pairs) {
      project_pair(source, target);
    }
  } else {
    const SymbolId value_symbol = state_->grammar.symbolId("V");
    for (const RelationEdge &edge : session_->relation().edges(value_symbol)) {
      project_pair(edge.source, edge.target);
    }
  }

  for (auto &[pointer, objects] : unique_objects) {
    address_objects_[pointer] =
        std::vector<std::size_t>(objects.begin(), objects.end());
  }
}

std::vector<std::size_t> AliasClient::pointsTo(std::size_t ptr) const {
  if (!session_ && !specialized_backend_) {
    throw std::logic_error("solve() has not been called");
  }
  if (ptr >= state_->graph.vertexCount()) {
    return {};
  }
  if (!points_to_valid_) {
    rebuildPointsTo();
  }
  if (ptr >= points_to_.size()) {
    return {};
  }
  std::set<std::size_t> result;
  for (const AbstractLocation &location : points_to_[ptr]) {
    const auto it = location_nodes_.find(location);
    if (it != location_nodes_.end()) {
      result.insert(it->second);
    }
  }
  return {result.begin(), result.end()};
}

std::optional<std::size_t> AliasClient::baseObject(std::size_t pointee) const {
  if (const auto it = pointee_bases_.find(pointee);
      it != pointee_bases_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool AliasClient::locationsOverlap(const AbstractLocation &lhs,
                                   const AbstractLocation &rhs) {
  if (lhs.base != rhs.base) {
    return false;
  }
  if (!lhs.modulus && !rhs.modulus) {
    return lhs.offset == rhs.offset;
  }
  if (!lhs.modulus) {
    return lhs.offset % *rhs.modulus == rhs.offset % *rhs.modulus;
  }
  if (!rhs.modulus) {
    return rhs.offset % *lhs.modulus == lhs.offset % *lhs.modulus;
  }
  const std::uint64_t common = std::gcd(*lhs.modulus, *rhs.modulus);
  return lhs.offset % common == rhs.offset % common;
}

bool AliasClient::pointsToOverlap(std::size_t lhs, std::size_t rhs) const {
  if (lhs >= points_to_.size() || rhs >= points_to_.size()) {
    return false;
  }
  for (const AbstractLocation &left : points_to_[lhs]) {
    for (const AbstractLocation &right : points_to_[rhs]) {
      if (locationsOverlap(left, right)) {
        return true;
      }
    }
  }
  return false;
}

LabeledGraph AliasClient::buildSpecializedAliasGraph() const {
  LabeledGraph lowered;
  for (const std::string &name : constraints_.nodeNames()) {
    lowered.addVertex(name);
  }

  std::unordered_map<std::size_t, std::vector<std::size_t>> dereference_nodes;
  for (const AliasConstraintEdge &edge : constraints_.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::Addr) {
      dereference_nodes[edge.target].push_back(edge.source);
    }
  }
  auto dereferences =
      [&](std::size_t pointer) -> const std::vector<std::size_t> & {
    auto &result = dereference_nodes[pointer];
    if (result.empty()) {
      const std::size_t dereference =
          lowered.addVertex("specialized_deref_" + std::to_string(pointer));
      addBidirectionalEdge(lowered, dereference, pointer, "addr", "addrbar");
      result.push_back(dereference);
    }
    return result;
  };

  for (const AliasConstraintEdge &edge : constraints_.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::MemoryTransfer) {
      const std::size_t temporary = lowered.addVertex(
          "specialized_memtransfer_" + std::to_string(edge.source) + "_" +
          std::to_string(edge.target));
      for (std::size_t object : dereferences(edge.source)) {
        addBidirectionalEdge(lowered, object, temporary, "copy", "copybar");
      }
      for (std::size_t object : dereferences(edge.target)) {
        addBidirectionalEdge(lowered, temporary, object, "copy", "copybar");
      }
      continue;
    }
    if (edge.kind == AliasConstraintEdgeKind::Store) {
      for (std::size_t object : dereferences(edge.target)) {
        addBidirectionalEdge(lowered, edge.source, object, "copy", "copybar");
      }
      continue;
    }
    if (edge.kind == AliasConstraintEdgeKind::Load) {
      for (std::size_t object : dereferences(edge.source)) {
        addBidirectionalEdge(lowered, object, edge.target, "copy", "copybar");
      }
      continue;
    }
    addBidirectionalEdge(lowered, edge.source, edge.target,
                         aliasForwardLabel(edge), aliasReverseLabel(edge));
  }
  return lowered;
}

void AliasClient::rebuildPointsTo() const {
  const std::size_t node_count = constraints_.nodeNames().size();
  points_to_.assign(node_count, {});
  location_nodes_.clear();
  pointee_bases_.clear();
  using LocationSet = llvm::SparseBitVector<>;
  std::map<AbstractLocation, unsigned> location_ids;
  std::vector<AbstractLocation> locations;
  std::vector<LocationSet> point_bits(node_count);
  std::vector<std::vector<unsigned>> point_order(node_count);
  std::unordered_map<unsigned, LocationSet> memory;
  std::unordered_map<std::size_t, std::vector<unsigned>> memory_cells_by_base;

  auto internLocation = [&](const AbstractLocation &location) {
    auto [it, inserted] = location_ids.try_emplace(location, 0);
    if (inserted) {
      if (locations.size() >= std::numeric_limits<unsigned>::max()) {
        throw std::overflow_error("Too many abstract alias locations");
      }
      it->second = static_cast<unsigned>(locations.size());
      locations.push_back(location);
    }
    return it->second;
  };
  auto insertPointId = [&](std::size_t pointer, unsigned location_id) {
    if (!point_bits[pointer].test_and_set(location_id)) {
      return false;
    }
    point_order[pointer].push_back(location_id);
    return true;
  };
  auto insertPoint = [&](std::size_t pointer,
                         const AbstractLocation &location) {
    return insertPointId(pointer, internLocation(location));
  };
  auto ensureMemoryCell = [&](unsigned location_id) {
    const bool inserted = memory.try_emplace(location_id).second;
    if (inserted) {
      memory_cells_by_base[locations[location_id].base].push_back(location_id);
    }
    return inserted;
  };
  auto insertAll = [](LocationSet &target, const LocationSet &source) {
    bool changed = false;
    for (unsigned location_id : source) {
      changed = target.test_and_set(location_id) || changed;
    }
    return changed;
  };

  for (const AliasConstraintEdge &edge : constraints_.edges()) {
    if (edge.kind == AliasConstraintEdgeKind::Addr) {
      insertPoint(edge.target, {edge.source, 0, std::nullopt});
    }
  }

  const auto &edges = constraints_.edges();
  std::vector<std::vector<std::size_t>> point_dependents(node_count);
  std::unordered_map<std::size_t, std::set<std::size_t>>
      memory_dependents_by_base;
  std::vector<std::size_t> processed_source_counts(edges.size(), 0);
  std::deque<std::size_t> worklist;
  std::vector<bool> queued(edges.size(), false);
  auto schedule = [&](std::size_t edge_index) {
    if (!queued[edge_index]) {
      queued[edge_index] = true;
      worklist.push_back(edge_index);
    }
  };
  for (std::size_t index = 0; index < edges.size(); ++index) {
    const AliasConstraintEdge &edge = edges[index];
    if (edge.kind != AliasConstraintEdgeKind::Addr) {
      schedule(index);
    }
    switch (edge.kind) {
    case AliasConstraintEdgeKind::Copy:
    case AliasConstraintEdgeKind::NormalGep:
    case AliasConstraintEdgeKind::VariantGep:
    case AliasConstraintEdgeKind::Load:
      point_dependents[edge.source].push_back(index);
      break;
    case AliasConstraintEdgeKind::Store:
    case AliasConstraintEdgeKind::MemoryTransfer:
      point_dependents[edge.source].push_back(index);
      point_dependents[edge.target].push_back(index);
      break;
    case AliasConstraintEdgeKind::Addr:
      break;
    }
  }

  while (!worklist.empty()) {
    const std::size_t edge_index = worklist.front();
    worklist.pop_front();
    queued[edge_index] = false;
    const AliasConstraintEdge &edge = edges[edge_index];
    bool point_changed = false;
    std::set<std::size_t> memory_changed_bases;
    switch (edge.kind) {
    case AliasConstraintEdgeKind::Addr:
      break;
    case AliasConstraintEdgeKind::Copy: {
      const std::size_t source_size = point_order[edge.source].size();
      for (std::size_t index = processed_source_counts[edge_index];
           index < source_size; ++index) {
        point_changed =
            insertPointId(edge.target, point_order[edge.source][index]) ||
            point_changed;
      }
      processed_source_counts[edge_index] = source_size;
      break;
    }
    case AliasConstraintEdgeKind::NormalGep:
    case AliasConstraintEdgeKind::VariantGep: {
      const std::size_t source_size = point_order[edge.source].size();
      for (std::size_t index = processed_source_counts[edge_index];
           index < source_size; ++index) {
        const AbstractLocation base =
            locations[point_order[edge.source][index]];
        AbstractLocation field = base;
        const std::uint64_t increment = edge.attribute.value_or(0);
        if (edge.kind == AliasConstraintEdgeKind::VariantGep) {
          const std::uint64_t edge_modulus = edge.modulus.value_or(1);
          field.modulus = field.modulus ? std::gcd(*field.modulus, edge_modulus)
                                        : edge_modulus;
        }
        if (field.modulus) {
          field.offset =
              (field.offset % *field.modulus + increment % *field.modulus) %
              *field.modulus;
        } else if (field.offset <=
                   std::numeric_limits<std::uint64_t>::max() - increment) {
          field.offset += increment;
        } else {
          field.offset = 0;
          field.modulus = 1;
        }
        point_changed = insertPoint(edge.target, field) || point_changed;
      }
      processed_source_counts[edge_index] = source_size;
      break;
    }
    case AliasConstraintEdgeKind::Store: {
      const LocationSet &value = point_bits[edge.source];
      for (unsigned destination_id : point_bits[edge.target]) {
        const AbstractLocation &destination = locations[destination_id];
        if (ensureMemoryCell(destination_id)) {
          memory_changed_bases.insert(destination.base);
        }
        for (unsigned cell_id : memory_cells_by_base[destination.base]) {
          const AbstractLocation &cell = locations[cell_id];
          if (locationsOverlap(destination, cell)) {
            if (insertAll(memory[cell_id], value)) {
              memory_changed_bases.insert(destination.base);
            }
          }
        }
      }
      break;
    }
    case AliasConstraintEdgeKind::Load:
      for (unsigned source_id : point_bits[edge.source]) {
        const AbstractLocation &source = locations[source_id];
        memory_dependents_by_base[source.base].insert(edge_index);
        const auto cells = memory_cells_by_base.find(source.base);
        if (cells == memory_cells_by_base.end()) {
          continue;
        }
        for (unsigned cell_id : cells->second) {
          const AbstractLocation &cell = locations[cell_id];
          if (locationsOverlap(source, cell)) {
            for (unsigned location_id : memory[cell_id]) {
              point_changed =
                  insertPointId(edge.target, location_id) || point_changed;
            }
          }
        }
      }
      break;
    case AliasConstraintEdgeKind::MemoryTransfer: {
      for (unsigned source_id : point_bits[edge.source]) {
        const AbstractLocation &source = locations[source_id];
        memory_dependents_by_base[source.base].insert(edge_index);
        const auto source_cells = memory_cells_by_base.find(source.base);
        if (source_cells == memory_cells_by_base.end()) {
          continue;
        }
        const std::vector<unsigned> snapshot = source_cells->second;
        for (unsigned destination_id : point_bits[edge.target]) {
          const AbstractLocation &destination = locations[destination_id];
          for (unsigned cell_id : snapshot) {
            const AbstractLocation &cell = locations[cell_id];
            AbstractLocation target_cell = destination;
            if (!source.modulus && !cell.modulus &&
                cell.offset >= source.offset) {
              const std::uint64_t relative = cell.offset - source.offset;
              if (edge.attribute && relative >= *edge.attribute) {
                continue;
              }
              if (target_cell.modulus) {
                target_cell.offset =
                    (target_cell.offset % *target_cell.modulus +
                     relative % *target_cell.modulus) %
                    *target_cell.modulus;
              } else if (target_cell.offset <=
                         std::numeric_limits<std::uint64_t>::max() - relative) {
                target_cell.offset += relative;
              } else {
                target_cell.offset = 0;
                target_cell.modulus = 1;
              }
            } else if (locationsOverlap(source, cell)) {
              // The exact relative field is unknown. Preserve soundness by
              // writing an arbitrary-offset cell rooted at the destination.
              target_cell.offset = 0;
              target_cell.modulus = 1;
            } else {
              continue;
            }
            const unsigned target_cell_id = internLocation(target_cell);
            if (ensureMemoryCell(target_cell_id)) {
              memory_changed_bases.insert(target_cell.base);
            }
            if (insertAll(memory[target_cell_id], memory[cell_id])) {
              memory_changed_bases.insert(target_cell.base);
            }
          }
        }
      }
      break;
    }
    }
    if (point_changed) {
      for (std::size_t dependent : point_dependents[edge.target]) {
        schedule(dependent);
      }
    }
    for (std::size_t base : memory_changed_bases) {
      if (const auto dependents = memory_dependents_by_base.find(base);
          dependents != memory_dependents_by_base.end()) {
        for (std::size_t dependent : dependents->second) {
          schedule(dependent);
        }
      }
    }
  }

  const std::size_t pointer_count = points_to_.size();
  std::size_t next_virtual_object = state_->graph.vertexCount();
  for (std::size_t pointer = 0; pointer < pointer_count; ++pointer) {
    for (unsigned location_id : point_bits[pointer]) {
      points_to_[pointer].insert(locations[location_id]);
    }
    for (const AbstractLocation &location : points_to_[pointer]) {
      if (location_nodes_.count(location) != 0) {
        continue;
      }
      std::size_t object_node = location.base;
      if (location.modulus || location.offset != 0) {
        object_node = next_virtual_object++;
      }
      location_nodes_.emplace(location, object_node);
      pointee_bases_[object_node] = location.base;
    }
  }
  points_to_valid_ = true;
}

} // namespace lotus::cfl::classical
