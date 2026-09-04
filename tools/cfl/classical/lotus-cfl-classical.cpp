#include "CFL/Classical/Core/Validation.h"
#include "CFL/Classical/Solvers/Preprocessing/GraphSimplification.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace lotus::cfl::classical;

namespace {

struct Options {
  std::string grammar;
  std::string graph;
  SolverBackend backend = SolverBackend::SparseSet;
  GraphLoadOptions graph_options;
  GrammarParseOptions grammar_options;
  bool dump_relation = false;
  bool start_only = false;
  bool count_only = false;
  bool json_stats = false;
  bool validate_only = false;
  bool unidirectional = false;
  bool simplify_focr_cycles = false;
  std::vector<std::pair<std::string, std::string>> pearl_inverse_relations;
  GraphSimplificationOptions simplification;
  std::string relation_output;
  std::string stats_output;
  std::string graph_output;
};

void usage(std::ostream &stream) {
  stream
      << "Usage: lotus-cfl-classical --grammar FILE --graph FILE [options]\n"
         "Options:\n"
         "  --solver sparse-set|sparse-bitvector|graspan|sqid|pearl|"
         "transitive-closure|pocr|hpocr|focr\n"
         "  --graph-mode plain|matrix|pag-matrix\n"
         "  --direction plain|reverse|bidirectional\n"
         "  --attribute-domain var:i=N,N,...  Variable-specific domain\n"
         "  --attribute-domain kind:call=N,N  Symbol-kind domain\n"
         "  --max-attribute-expansions N      Expansion safety limit\n"
         "  --dump-relation\n"
         "  --relation-output FILE\n"
         "  --graph-output FILE          Write normalized/preprocessed graph\n"
         "  --start-only\n"
         "  --count-only                 Emit POCR Count symbols only\n"
         "  --json-stats\n"
         "  --stats-output FILE\n"
         "  --unidirectional            Honor POCR Insert/Follow metadata\n"
         "  --focr-scc                  Simplify FOCR critical-graph SCCs\n"
         "  --pearl-inverse X,XBAR      Pair inverse PEARL relations\n"
         "  --simplification-flavor alias|value-flow\n"
         "  --scc-elimination           Condense direct-edge SCCs\n"
         "  --graph-folding             Apply POCR client graph folding\n"
         "  --interdyck-pruning         Prune non-contributing Dyck edges\n"
         "  --simplify-graph            Enable SCC elimination and folding\n"
         "  --validate-only\n";
}

std::vector<std::uint32_t> parseAttributes(const std::string &value) {
  std::vector<std::uint32_t> result;
  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, ',')) {
    const auto attribute = parseAttributeValue(token);
    if (!attribute) {
      throw std::invalid_argument("Invalid attribute: " + token);
    }
    result.push_back(*attribute);
  }
  return result;
}

void parseAttributeDomain(const std::string &spec,
                          GrammarParseOptions &options) {
  const auto separator = spec.find('=');
  if (separator == std::string::npos || separator + 1 == spec.size()) {
    throw std::invalid_argument(
        "Attribute domain must have the form var:i=1,2 or kind:call=1,2");
  }
  const std::string name = spec.substr(0, separator);
  const auto domain = parseAttributes(spec.substr(separator + 1));
  if (name.rfind("var:", 0) == 0 && name.size() == 5) {
    options.variable_attributes[name.back()] = domain;
    return;
  }
  if (name.rfind("kind:", 0) == 0 && name.size() > 5) {
    options.symbol_attributes[name.substr(5)] = domain;
    return;
  }
  throw std::invalid_argument(
      "Attribute domain must have the form var:i=1,2 or kind:call=1,2");
}

GrammarParseOptions mergeGrammarOptions(GrammarParseOptions inferred,
                                        const GrammarParseOptions &explicit_) {
  for (const auto &[variable, domain] : explicit_.variable_attributes) {
    inferred.variable_attributes[variable] = domain;
  }
  for (const auto &[kind, domain] : explicit_.symbol_attributes) {
    inferred.symbol_attributes[kind] = domain;
  }
  inferred.max_attribute_expansions = explicit_.max_attribute_expansions;
  return inferred;
}

std::ostream *openOutput(const std::string &path, std::ofstream &file) {
  if (path.empty() || path == "-") {
    return &std::cout;
  }
  file.open(path);
  if (!file) {
    throw std::runtime_error("Failed to open output file: " + path);
  }
  return &file;
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    auto value = [&]() -> std::string {
      if (++index >= argc) {
        throw std::invalid_argument("Missing value for " + argument);
      }
      return argv[index];
    };

    if (argument == "--grammar") {
      options.grammar = value();
    } else if (argument == "--graph") {
      options.graph = value();
    } else if (argument == "--solver") {
      options.backend = parseSolverBackend(value());
    } else if (argument == "--graph-mode") {
      const std::string selected = value();
      if (selected == "plain") {
        options.graph_options.mode = GraphMode::Plain;
      } else if (selected == "matrix") {
        options.graph_options.mode = GraphMode::Matrix;
      } else if (selected == "pag-matrix") {
        options.graph_options.mode = GraphMode::PAGMatrix;
      } else {
        throw std::invalid_argument("Unknown graph mode: " + selected);
      }
    } else if (argument == "--direction") {
      const std::string selected = value();
      if (selected == "plain") {
        options.graph_options.direction = EdgeDirection::Plain;
      } else if (selected == "reverse") {
        options.graph_options.direction = EdgeDirection::Reverse;
      } else if (selected == "bidirectional") {
        options.graph_options.direction = EdgeDirection::Bidirectional;
      } else {
        throw std::invalid_argument("Unknown direction: " + selected);
      }
    } else if (argument == "--attribute-domain") {
      parseAttributeDomain(value(), options.grammar_options);
    } else if (argument == "--max-attribute-expansions") {
      options.grammar_options.max_attribute_expansions = std::stoull(value());
    } else if (argument == "--dump-relation") {
      options.dump_relation = true;
    } else if (argument == "--relation-output") {
      options.relation_output = value();
      options.dump_relation = true;
    } else if (argument == "--graph-output") {
      options.graph_output = value();
    } else if (argument == "--start-only") {
      options.start_only = true;
    } else if (argument == "--count-only") {
      options.count_only = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--stats-output") {
      options.stats_output = value();
    } else if (argument == "--unidirectional") {
      options.unidirectional = true;
    } else if (argument == "--focr-scc") {
      options.simplify_focr_cycles = true;
    } else if (argument == "--pearl-inverse") {
      const std::string relation_pair = value();
      const std::size_t comma = relation_pair.find(',');
      if (comma == std::string::npos || comma == 0 ||
          comma + 1 == relation_pair.size()) {
        throw std::invalid_argument("PEARL inverse relation must be X,XBAR");
      }
      options.pearl_inverse_relations.emplace_back(
          relation_pair.substr(0, comma), relation_pair.substr(comma + 1));
    } else if (argument == "--simplification-flavor") {
      const std::string selected = value();
      if (selected == "alias") {
        options.simplification.flavor = GraphSimplificationFlavor::Alias;
      } else if (selected == "value-flow") {
        options.simplification.flavor = GraphSimplificationFlavor::ValueFlow;
      } else {
        throw std::invalid_argument("Unknown simplification flavor: " +
                                    selected);
      }
    } else if (argument == "--scc-elimination") {
      options.simplification.eliminate_sccs = true;
    } else if (argument == "--graph-folding") {
      options.simplification.fold_graph = true;
    } else if (argument == "--interdyck-pruning") {
      options.simplification.prune_interdyck = true;
    } else if (argument == "--simplify-graph") {
      options.simplification.eliminate_sccs = true;
      options.simplification.fold_graph = true;
    } else if (argument == "--validate-only") {
      options.validate_only = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown option: " + argument);
    }
  }
  if (options.grammar.empty() || options.graph.empty()) {
    throw std::invalid_argument("--grammar and --graph are required");
  }
  if (options.start_only && options.count_only) {
    throw std::invalid_argument("--start-only and --count-only are exclusive");
  }
  return options;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    const auto total_start = std::chrono::steady_clock::now();
    const auto graph_start = std::chrono::steady_clock::now();
    LabeledGraph graph =
        LabeledGraph::parseFromFile(options.graph, options.graph_options);
    GraphSimplificationStatistics simplification_stats;
    simplification_stats.original_nodes = graph.vertexCount();
    simplification_stats.original_edges = graph.edgeCount();
    simplification_stats.reduced_nodes = graph.vertexCount();
    simplification_stats.reduced_edges = graph.edgeCount();
    if (options.simplification.eliminate_sccs ||
        options.simplification.fold_graph ||
        options.simplification.prune_interdyck) {
      GraphSimplificationResult simplified =
          simplifyGraph(graph, options.simplification);
      simplification_stats = simplified.statistics;
      graph = std::move(simplified.graph);
    }
    if (!options.graph_output.empty()) {
      graph.writeTextFile(options.graph_output);
    }
    const auto graph_load_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - graph_start)
            .count();

    const GrammarParseOptions grammar_options = mergeGrammarOptions(
        inferGrammarAttributes(graph), options.grammar_options);
    const auto grammar_start = std::chrono::steady_clock::now();
    const Grammar grammar =
        Grammar::parseFromFile(options.grammar, grammar_options);
    const auto grammar_load_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - grammar_start)
            .count();

    bool invalid = false;
    for (const GrammarIssue &issue : validateGraph(graph, grammar)) {
      const bool error = issue.severity == GrammarIssueSeverity::Error;
      std::cerr << (error ? "error: " : "warning: ") << issue.message << '\n';
      invalid = invalid || error;
    }
    if (invalid) {
      return 2;
    }
    if (options.validate_only) {
      std::cout << "validation=ok nodes=" << graph.vertexCount()
                << " symbols=" << grammar.symbolCount()
                << " productions=" << grammar.productionCount() << '\n';
      return 0;
    }

    SolverSession session(graph, grammar,
                          SolverOptions{options.backend, options.unidirectional,
                                        options.simplify_focr_cycles,
                                        options.pearl_inverse_relations});
    const ReachabilityStats stats = session.solve();
    if (options.dump_relation) {
      std::ofstream relation_file;
      std::ostream &relation_output =
          *openOutput(options.relation_output, relation_file);
      auto edges = session.relation().edges();
      std::sort(edges.begin(), edges.end(),
                [](const RelationEdge &lhs, const RelationEdge &rhs) {
                  if (lhs.source != rhs.source) {
                    return lhs.source < rhs.source;
                  }
                  if (lhs.target != rhs.target) {
                    return lhs.target < rhs.target;
                  }
                  return lhs.symbol < rhs.symbol;
                });
      for (const RelationEdge &edge : edges) {
        if (options.start_only && edge.symbol != grammar.startSymbolId()) {
          continue;
        }
        if (options.count_only &&
            !grammar.isCountSymbol(grammar.symbolName(edge.symbol))) {
          continue;
        }
        if (options.count_only && edge.source == edge.target) {
          continue;
        }
        relation_output << graph.vertexName(edge.source) << ','
                        << graph.vertexName(edge.target) << ','
                        << grammar.symbolName(edge.symbol) << '\n';
      }
    }

    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - total_start)
                              .count();
    std::ofstream stats_file;
    std::ostream &stats_output = *openOutput(options.stats_output, stats_file);
    if (options.json_stats) {
      stats_output
          << "{\"solver\":\"" << solverBackendName(options.backend)
          << "\",\"nodes\":" << graph.vertexCount()
          << ",\"base_edges\":" << stats.base_graph_edges
          << ",\"grammar_symbols\":" << stats.grammar_symbols
          << ",\"grammar_terminals\":" << stats.grammar_terminals
          << ",\"grammar_nonterminals\":" << stats.grammar_nonterminals
          << ",\"grammar_productions\":" << stats.grammar_productions
          << ",\"grammar_nullable\":" << stats.grammar_nullable_symbols
          << ",\"grammar_transitive\":" << stats.grammar_transitive_symbols
          << ",\"input_edges\":" << stats.input_edges
          << ",\"derived_edges\":" << stats.added_edges
          << ",\"relation_edges\":" << stats.relation_edges
          << ",\"start_edges\":" << stats.start_symbol_edges
          << ",\"count_edges\":" << stats.count_symbol_edges
          << ",\"checks\":" << stats.classical_iterations
          << ",\"processed_items\":" << stats.processed_work_items
          << ",\"duplicates\":" << stats.duplicate_edges
          << ",\"peak_worklist\":" << stats.peak_worklist_size
          << ",\"candidate_relation_edges\":" << stats.candidate_relation_edges
          << ",\"unidirectional\":"
          << (options.unidirectional ? "true" : "false")
          << ",\"relation_payload_bytes_estimate\":"
          << stats.relation_payload_bytes_estimate
          << ",\"transitive_instances\":" << stats.transitive_closure_instances
          << ",\"transitive_edges\":" << stats.transitive_relation_edges
          << ",\"transitive_arcs\":" << stats.transitive_arc_insertions
          << ",\"transitive_propagated_pairs\":"
          << stats.transitive_propagated_pairs
          << ",\"transitive_duplicate_pairs\":"
          << stats.transitive_duplicate_pairs
          << ",\"transitive_payload_bytes_estimate\":"
          << stats.transitive_payload_bytes_estimate
          << ",\"pocr_tree_roots\":" << stats.pocr_tree_roots
          << ",\"pocr_tree_nodes\":" << stats.pocr_tree_nodes
          << ",\"pocr_tree_edges\":" << stats.pocr_tree_edges
          << ",\"pocr_traversal_steps\":" << stats.pocr_traversal_steps
          << ",\"pocr_tree_join_visits\":" << stats.pocr_tree_join_visits
          << ",\"focr_critical_edges\":" << stats.fully_ordered_critical_edges
          << ",\"focr_reachability_checks\":"
          << stats.fully_ordered_reachability_checks
          << ",\"focr_tree_join_visits\":"
          << stats.fully_ordered_tree_join_visits
          << ",\"focr_critical_edge_insertions\":"
          << stats.fully_ordered_critical_edge_insertions
          << ",\"focr_critical_edge_removals\":"
          << stats.fully_ordered_critical_edge_removals
          << ",\"focr_cycle_simplifications\":"
          << stats.fully_ordered_cycle_simplifications
          << ",\"graspan_epochs\":" << stats.graspan_epochs
          << ",\"simplified_nodes\":" << simplification_stats.reduced_nodes
          << ",\"scc_nodes_merged\":" << simplification_stats.scc_nodes_merged
          << ",\"folded_nodes\":" << simplification_stats.folded_nodes
          << ",\"common_dereference_nodes_merged\":"
          << simplification_stats.common_dereference_nodes_merged
          << ",\"interdyck_edges_pruned\":"
          << simplification_stats.interdyck_edges_pruned
          << ",\"graph_load_us\":" << graph_load_us
          << ",\"grammar_load_us\":" << grammar_load_us
          << ",\"solve_us\":" << stats.solve_time_microseconds
          << ",\"total_us\":" << total_us
          << ",\"rounds\":" << stats.solver_rounds << "}\n";
    } else {
      stats_output
          << "solver=" << solverBackendName(options.backend)
          << " nodes=" << graph.vertexCount()
          << " base_edges=" << stats.base_graph_edges
          << " grammar_symbols=" << stats.grammar_symbols
          << " productions=" << stats.grammar_productions
          << " input_edges=" << stats.input_edges
          << " derived_edges=" << stats.added_edges
          << " relation_edges=" << stats.relation_edges
          << " start_edges=" << stats.start_symbol_edges
          << " count_edges=" << stats.count_symbol_edges
          << " checks=" << stats.classical_iterations
          << " processed_items=" << stats.processed_work_items
          << " duplicates=" << stats.duplicate_edges
          << " peak_worklist=" << stats.peak_worklist_size
          << " candidate_relation_edges=" << stats.candidate_relation_edges
          << " unidirectional=" << options.unidirectional
          << " relation_payload_bytes_estimate="
          << stats.relation_payload_bytes_estimate
          << " transitive_instances=" << stats.transitive_closure_instances
          << " transitive_pairs=" << stats.transitive_propagated_pairs
          << " transitive_payload_bytes_estimate="
          << stats.transitive_payload_bytes_estimate
          << " pocr_tree_nodes=" << stats.pocr_tree_nodes
          << " pocr_traversal_steps=" << stats.pocr_traversal_steps
          << " pocr_tree_join_visits=" << stats.pocr_tree_join_visits
          << " focr_critical_edges=" << stats.fully_ordered_critical_edges
          << " focr_reachability_checks="
          << stats.fully_ordered_reachability_checks
          << " focr_tree_join_visits=" << stats.fully_ordered_tree_join_visits
          << " graspan_epochs=" << stats.graspan_epochs
          << " simplified_nodes=" << simplification_stats.reduced_nodes
          << " scc_nodes_merged=" << simplification_stats.scc_nodes_merged
          << " folded_nodes=" << simplification_stats.folded_nodes
          << " interdyck_edges_pruned="
          << simplification_stats.interdyck_edges_pruned
          << " graph_load_us=" << graph_load_us
          << " grammar_load_us=" << grammar_load_us
          << " solve_us=" << stats.solve_time_microseconds
          << " total_us=" << total_us << " rounds=" << stats.solver_rounds
          << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
