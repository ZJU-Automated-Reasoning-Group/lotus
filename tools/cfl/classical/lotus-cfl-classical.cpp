#include "CFL/Classical/Core/Validation.h"
#include "CFL/Classical/Solvers/Reachability.h"

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
  bool json_stats = false;
  bool validate_only = false;
  std::string relation_output;
  std::string stats_output;
};

void usage(std::ostream &stream) {
  stream << "Usage: lotus-cfl-classical --grammar FILE --graph FILE [options]\n"
            "Options:\n"
            "  --solver sparse-set|sparse-bitvector|transitive-closure\n"
            "  --graph-mode plain|matrix|pag-matrix\n"
            "  --direction plain|reverse|bidirectional\n"
            "  --attribute-domain var:i=N,N,...  Variable-specific domain\n"
            "  --attribute-domain kind:call=N,N  Symbol-kind domain\n"
            "  --max-attribute-expansions N      Expansion safety limit\n"
            "  --dump-relation\n"
            "  --relation-output FILE\n"
            "  --start-only\n"
            "  --json-stats\n"
            "  --stats-output FILE\n"
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
      const std::string selected = value();
      if (selected == "sparse-set") {
        options.backend = SolverBackend::SparseSet;
      } else if (selected == "sparse-bitvector") {
        options.backend = SolverBackend::SparseBitVector;
      } else if (selected == "transitive-closure") {
        options.backend = SolverBackend::TransitiveClosure;
      } else {
        throw std::invalid_argument("Unknown solver: " + selected);
      }
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
    } else if (argument == "--start-only") {
      options.start_only = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--stats-output") {
      options.stats_output = value();
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

    SolverSession session(graph, grammar, options.backend);
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
          << ",\"checks\":" << stats.classical_iterations
          << ",\"processed_items\":" << stats.processed_work_items
          << ",\"duplicates\":" << stats.duplicate_edges
          << ",\"peak_worklist\":" << stats.peak_worklist_size
          << ",\"relation_bytes_estimate\":" << stats.relation_memory_bytes
          << ",\"transitive_instances\":" << stats.transitive_closure_instances
          << ",\"transitive_edges\":" << stats.transitive_relation_edges
          << ",\"transitive_arcs\":" << stats.transitive_arc_insertions
          << ",\"transitive_propagated_pairs\":"
          << stats.transitive_propagated_pairs
          << ",\"transitive_duplicate_pairs\":"
          << stats.transitive_duplicate_pairs
          << ",\"transitive_bytes_estimate\":" << stats.transitive_memory_bytes
          << ",\"graph_load_us\":" << graph_load_us
          << ",\"grammar_load_us\":" << grammar_load_us
          << ",\"solve_us\":" << stats.solve_time_microseconds
          << ",\"total_us\":" << total_us
          << ",\"rounds\":" << stats.solver_rounds << "}\n";
    } else {
      stats_output << "solver=" << solverBackendName(options.backend)
                   << " nodes=" << graph.vertexCount()
                   << " base_edges=" << stats.base_graph_edges
                   << " grammar_symbols=" << stats.grammar_symbols
                   << " productions=" << stats.grammar_productions
                   << " input_edges=" << stats.input_edges
                   << " derived_edges=" << stats.added_edges
                   << " relation_edges=" << stats.relation_edges
                   << " start_edges=" << stats.start_symbol_edges
                   << " checks=" << stats.classical_iterations
                   << " processed_items=" << stats.processed_work_items
                   << " duplicates=" << stats.duplicate_edges
                   << " peak_worklist=" << stats.peak_worklist_size
                   << " relation_bytes_estimate=" << stats.relation_memory_bytes
                   << " transitive_instances="
                   << stats.transitive_closure_instances
                   << " transitive_pairs=" << stats.transitive_propagated_pairs
                   << " transitive_bytes_estimate="
                   << stats.transitive_memory_bytes
                   << " graph_load_us=" << graph_load_us
                   << " grammar_load_us=" << grammar_load_us
                   << " solve_us=" << stats.solve_time_microseconds
                   << " total_us=" << total_us
                   << " rounds=" << stats.solver_rounds << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
