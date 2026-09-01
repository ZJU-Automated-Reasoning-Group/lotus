#include "CFL/Classical/Solver.h"
#include "CFL/Classical/Validation.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
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
  SolverBackend backend = SolverBackend::Baseline;
  GraphLoadOptions graph_options;
  GrammarParseOptions grammar_options;
  bool dump_relation = false;
  bool json_stats = false;
};

void usage(std::ostream &stream) {
  stream << "Usage: lotus-cfl-classical --grammar FILE --graph FILE [options]\n"
            "Options:\n"
            "  --solver baseline|pocr|hybrid\n"
            "  --graph-mode plain|matrix|pag-matrix\n"
            "  --direction plain|reverse|bidirectional\n"
            "  --attributes N,N,...\n"
            "  --dump-relation\n"
            "  --json-stats\n";
}

std::vector<std::uint32_t> parseAttributes(const std::string &value) {
  std::vector<std::uint32_t> result;
  std::stringstream stream(value);
  std::string token;
  while (std::getline(stream, token, ',')) {
    result.push_back(static_cast<std::uint32_t>(std::stoul(token)));
  }
  return result;
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
      if (selected == "baseline") {
        options.backend = SolverBackend::Baseline;
      } else if (selected == "pocr") {
        options.backend = SolverBackend::POCR;
      } else if (selected == "hybrid") {
        options.backend = SolverBackend::Hybrid;
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
    } else if (argument == "--attributes") {
      options.grammar_options.attributes = parseAttributes(value());
    } else if (argument == "--dump-relation") {
      options.dump_relation = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
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
    const Grammar grammar =
        Grammar::parseFromFile(options.grammar, options.grammar_options);
    LabeledGraph graph =
        LabeledGraph::parseFromFile(options.graph, options.graph_options);

    bool invalid = false;
    for (const GrammarIssue &issue : validateGraph(graph, grammar)) {
      const bool error = issue.severity == GrammarIssueSeverity::Error;
      std::cerr << (error ? "error: " : "warning: ") << issue.message << '\n';
      invalid = invalid || error;
    }
    if (invalid) {
      return 2;
    }

    SolverSession session(graph, grammar, options.backend);
    const ReachabilityStats stats = session.solve();
    if (options.dump_relation) {
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
        std::cout << graph.vertexName(edge.source) << ','
                  << graph.vertexName(edge.target) << ','
                  << grammar.symbolName(edge.symbol) << '\n';
      }
    }

    if (options.json_stats) {
      std::cout << "{\"solver\":\"" << solverBackendName(options.backend)
                << "\",\"nodes\":" << graph.vertexCount()
                << ",\"input_edges\":" << stats.input_edges
                << ",\"derived_edges\":" << stats.added_edges
                << ",\"relation_edges\":" << stats.relation_edges
                << ",\"checks\":" << stats.classical_iterations
                << ",\"solve_us\":" << stats.solve_time_microseconds
                << ",\"rounds\":" << stats.solver_rounds << "}\n";
    } else {
      std::cout << "solver=" << solverBackendName(options.backend)
                << " nodes=" << graph.vertexCount()
                << " input_edges=" << stats.input_edges
                << " derived_edges=" << stats.added_edges
                << " relation_edges=" << stats.relation_edges
                << " checks=" << stats.classical_iterations
                << " solve_us=" << stats.solve_time_microseconds
                << " rounds=" << stats.solver_rounds << '\n';
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
