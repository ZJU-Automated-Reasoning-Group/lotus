#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/Engines/POCR/ClientGrammars.h"
#include "CFL/Classical/Solvers/Engines/POCR/SpecializedEngines.h"
#include "CFL/Classical/Solvers/Preprocessing/GraphSimplification.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace lotus::cfl::classical;
using namespace lotus::cfl::classical::engines;

namespace {

enum class Engine {
  StdAA,
  GspanAA,
  GrAA,
  GrGspanAA,
  PocrAA,
  FocrAA,
  StdVFA,
  GspanVFA,
  GrVFA,
  GrGspanVFA,
  PocrVFA,
  FocrVFA,
};

struct Options {
  std::string graph;
  Engine engine = Engine::PocrAA;
  bool engine_selected = false;
  bool dump_pairs = false;
  bool json_stats = false;
  bool simplify_focr_cycles = false;
  bool eliminate_sccs = false;
  bool fold_graph = false;
  bool prune_interdyck = false;
  std::string graph_output;
  std::string query_source;
  std::string query_target;
};

void usage(std::ostream &output) {
  output << "Usage: lotus-cfl-pocr --engine "
            "std-aa|gspan-aa|gr-aa|grgspan-aa|pocr-aa|focr-aa|\n"
            "       std-vfa|gspan-vfa|gr-vfa|grgspan-vfa|pocr-vfa|focr-vfa "
            "--graph FILE [options]\n"
            "Options:\n"
            "  --query SOURCE,TARGET\n"
            "  --focr-scc\n"
            "  --scc\n"
            "  --graph-folding\n"
            "  --interdyck\n"
            "  --simplify-graph          Enable SCC elimination and folding\n"
            "  --graph-output FILE       Write the preprocessed graph\n"
            "  --dump-pairs\n"
            "  --json-stats\n";
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
    if (argument == "--engine") {
      const std::string selected = value();
      options.engine_selected = true;
      if (selected == "std-aa") {
        options.engine = Engine::StdAA;
      } else if (selected == "gspan-aa") {
        options.engine = Engine::GspanAA;
      } else if (selected == "gr-aa") {
        options.engine = Engine::GrAA;
      } else if (selected == "grgspan-aa") {
        options.engine = Engine::GrGspanAA;
      } else if (selected == "pocr-aa") {
        options.engine = Engine::PocrAA;
      } else if (selected == "focr-aa") {
        options.engine = Engine::FocrAA;
      } else if (selected == "std-vfa") {
        options.engine = Engine::StdVFA;
      } else if (selected == "gspan-vfa") {
        options.engine = Engine::GspanVFA;
      } else if (selected == "gr-vfa") {
        options.engine = Engine::GrVFA;
      } else if (selected == "grgspan-vfa") {
        options.engine = Engine::GrGspanVFA;
      } else if (selected == "pocr-vfa") {
        options.engine = Engine::PocrVFA;
      } else if (selected == "focr-vfa") {
        options.engine = Engine::FocrVFA;
      } else {
        throw std::invalid_argument("Unknown POCR engine: " + selected);
      }
    } else if (argument == "--graph") {
      options.graph = value();
    } else if (argument == "--query") {
      const std::string query = value();
      const auto comma = query.find(',');
      if (comma == std::string::npos || comma == 0 ||
          comma + 1 == query.size()) {
        throw std::invalid_argument("Query must be SOURCE,TARGET");
      }
      options.query_source = query.substr(0, comma);
      options.query_target = query.substr(comma + 1);
    } else if (argument == "--dump-pairs") {
      options.dump_pairs = true;
    } else if (argument == "--focr-scc") {
      options.simplify_focr_cycles = true;
    } else if (argument == "--scc") {
      options.eliminate_sccs = true;
    } else if (argument == "--graph-folding") {
      options.fold_graph = true;
    } else if (argument == "--interdyck") {
      options.prune_interdyck = true;
    } else if (argument == "--simplify-graph") {
      options.eliminate_sccs = true;
      options.fold_graph = true;
    } else if (argument == "--graph-output") {
      options.graph_output = value();
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown option: " + argument);
    }
  }
  if (!options.engine_selected || options.graph.empty()) {
    throw std::invalid_argument("--engine and --graph are required");
  }
  return options;
}

const char *engineName(Engine engine) {
  switch (engine) {
  case Engine::StdAA:
    return "std-aa";
  case Engine::GspanAA:
    return "gspan-aa";
  case Engine::GrAA:
    return "gr-aa";
  case Engine::GrGspanAA:
    return "grgspan-aa";
  case Engine::PocrAA:
    return "pocr-aa";
  case Engine::FocrAA:
    return "focr-aa";
  case Engine::StdVFA:
    return "std-vfa";
  case Engine::GspanVFA:
    return "gspan-vfa";
  case Engine::GrVFA:
    return "gr-vfa";
  case Engine::GrGspanVFA:
    return "grgspan-vfa";
  case Engine::PocrVFA:
    return "pocr-vfa";
  case Engine::FocrVFA:
    return "focr-vfa";
  }
  return "unknown";
}

bool isAliasEngine(Engine engine) {
  return engine == Engine::StdAA || engine == Engine::GspanAA ||
         engine == Engine::GrAA || engine == Engine::GrGspanAA ||
         engine == Engine::PocrAA || engine == Engine::FocrAA;
}

bool isGrammarEngine(Engine engine) {
  return engine == Engine::StdAA || engine == Engine::GspanAA ||
         engine == Engine::GrAA || engine == Engine::GrGspanAA ||
         engine == Engine::StdVFA || engine == Engine::GspanVFA ||
         engine == Engine::GrVFA || engine == Engine::GrGspanVFA;
}

PocrClientGrammar clientGrammar(Engine engine) {
  switch (engine) {
  case Engine::StdAA:
  case Engine::GspanAA:
    return PocrClientGrammar::StandardAlias;
  case Engine::GrAA:
  case Engine::GrGspanAA:
    return PocrClientGrammar::RewrittenAlias;
  case Engine::StdVFA:
  case Engine::GspanVFA:
    return PocrClientGrammar::StandardValueFlow;
  case Engine::GrVFA:
  case Engine::GrGspanVFA:
    return PocrClientGrammar::RewrittenValueFlow;
  default:
    throw std::logic_error("Specialized POCR engine has no client grammar");
  }
}

SolverBackend grammarBackend(Engine engine) {
  return engine == Engine::GspanAA || engine == Engine::GrGspanAA ||
                 engine == Engine::GspanVFA || engine == Engine::GrGspanVFA
             ? SolverBackend::Graspan
             : SolverBackend::SparseBitVector;
}

SpecializedPocrStatistics genericStatistics(const ReachabilityStats &source) {
  SpecializedPocrStatistics result;
  result.graph_nodes = source.graph_nodes;
  result.graph_edges = source.input_edges;
  result.processed_items = source.processed_work_items;
  result.queued_items = source.added_edges;
  result.duplicate_items = source.duplicate_edges;
  result.reachability_checks = source.classical_iterations;
  result.reachability_pairs = source.relation_edges;
  result.value_or_flow_pairs = source.start_symbol_edges;
  result.critical_edges = source.fully_ordered_critical_edges;
  result.cycle_simplifications = source.fully_ordered_cycle_simplifications;
  return result;
}

LabeledGraph completePocrAliasEdges(const LabeledGraph &graph) {
  LabeledGraph completed = graph;
  for (const LabeledEdge &edge : graph.edges()) {
    if (edge.label != "a" && edge.label != "d" &&
        edge.label.rfind("f_", 0) != 0) {
      continue;
    }
    completed.addEdge(edge.target, edge.source,
                      LabeledGraph::complementLabel(edge.label));
  }
  return completed;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    const LabeledGraph input_graph = LabeledGraph::parseFromFile(
        options.graph,
        GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
    LabeledGraph graph = input_graph;
    GraphSimplificationStatistics simplification;
    std::vector<NodeId> representative(input_graph.vertexCount());
    for (NodeId node = 0; node < representative.size(); ++node) {
      representative[node] = node;
    }
    if (options.eliminate_sccs || options.fold_graph ||
        options.prune_interdyck) {
      const bool alias = isAliasEngine(options.engine);
      GraphSimplificationResult reduced = simplifyGraph(
          input_graph, {alias ? GraphSimplificationFlavor::Alias
                              : GraphSimplificationFlavor::ValueFlow,
                        options.eliminate_sccs, options.fold_graph,
                        options.prune_interdyck});
      graph = std::move(reduced.graph);
      representative = std::move(reduced.representative);
      simplification = reduced.statistics;
    } else {
      simplification.original_nodes = input_graph.vertexCount();
      simplification.original_edges = input_graph.edgeCount();
      simplification.reduced_nodes = graph.vertexCount();
      simplification.reduced_edges = graph.edgeCount();
    }
    if (!options.graph_output.empty()) {
      graph.writeTextFile(options.graph_output);
    }

    SpecializedPocrStatistics statistics;
    std::vector<std::pair<NodeId, NodeId>> pairs;
    bool query_result = false;
    const bool has_query = !options.query_source.empty();
    const NodeId query_source =
        has_query
            ? representative.at(input_graph.vertexId(options.query_source))
            : 0;
    const NodeId query_target =
        has_query
            ? representative.at(input_graph.vertexId(options.query_target))
            : 0;

    if (isGrammarEngine(options.engine)) {
      LabeledGraph grammar_graph =
          isAliasEngine(options.engine) ? completePocrAliasEdges(graph) : graph;
      const Grammar grammar =
          buildPocrClientGrammar(clientGrammar(options.engine), grammar_graph);
      SolverSession engine(grammar_graph, grammar,
                           grammarBackend(options.engine));
      statistics = genericStatistics(engine.solve());
      for (const RelationEdge &edge :
           engine.relation().edges(grammar.startSymbolId())) {
        pairs.emplace_back(edge.source, edge.target);
      }
      query_result = has_query && engine.contains(query_source, query_target,
                                                  grammar.startSymbol());
    } else if (options.engine == Engine::PocrAA) {
      PocrAliasEngine engine(graph);
      statistics = engine.solve();
      pairs = engine.valuePairs();
      query_result = has_query && engine.mayAlias(query_source, query_target);
    } else if (options.engine == Engine::FocrAA) {
      FocrAliasEngine engine(graph, options.simplify_focr_cycles);
      statistics = engine.solve();
      pairs = engine.valuePairs();
      query_result = has_query && engine.mayAlias(query_source, query_target);
    } else if (options.engine == Engine::PocrVFA) {
      PocrValueFlowEngine engine(graph);
      statistics = engine.solve();
      pairs = engine.flowPairs();
      query_result = has_query && engine.hasFlow(query_source, query_target);
    } else {
      FocrValueFlowEngine engine(graph, options.simplify_focr_cycles);
      statistics = engine.solve();
      pairs = engine.flowPairs();
      query_result = has_query && engine.hasFlow(query_source, query_target);
    }

    if (has_query) {
      std::cout << "reachable=" << (query_result ? "yes" : "no") << '\n';
    }
    if (options.dump_pairs) {
      std::sort(pairs.begin(), pairs.end());
      for (const auto &[source, target] : pairs) {
        std::cout << graph.vertexName(source) << ',' << graph.vertexName(target)
                  << '\n';
      }
    }
    if (options.json_stats) {
      std::cout << "{\"engine\":\"" << engineName(options.engine)
                << "\",\"nodes\":" << statistics.graph_nodes
                << ",\"input_edges\":" << statistics.graph_edges
                << ",\"processed_items\":" << statistics.processed_items
                << ",\"reachability_pairs\":" << statistics.reachability_pairs
                << ",\"result_pairs\":" << statistics.value_or_flow_pairs
                << ",\"matched_pairs\":" << statistics.matched_pairs
                << ",\"critical_edges\":" << statistics.critical_edges
                << ",\"cycle_simplifications\":"
                << statistics.cycle_simplifications
                << ",\"scc_nodes_merged\":" << simplification.scc_nodes_merged
                << ",\"folded_nodes\":" << simplification.folded_nodes
                << ",\"common_dereference_nodes_merged\":"
                << simplification.common_dereference_nodes_merged
                << ",\"interdyck_edges_pruned\":"
                << simplification.interdyck_edges_pruned << "}\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
