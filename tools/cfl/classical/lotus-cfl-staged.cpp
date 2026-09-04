#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Core/Relation.h"
#include "CFL/Classical/Core/Validation.h"
#include "CFL/Classical/Solvers/Engines/STG/StagedSolver.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace lotus::cfl::classical;
namespace stg = lotus::cfl::classical::engines::stg;

namespace {

enum class Mode { StandardDyck, ExtendedDyck, Alias };

struct Options {
  std::string grammar;
  std::string graph;
  Mode mode = Mode::StandardDyck;
  bool mode_selected = false;
  std::string summary;
  std::vector<std::string> delimiters;
  std::vector<std::string> neutral;
  std::string open;
  std::string close;
  std::string reverse_forward;
  std::string center;
  std::string backward;
  std::vector<std::string> phase_l;
  std::vector<std::string> phase_r;
  bool dump_relation = false;
  bool start_only = false;
  bool json_stats = false;
};

void usage(std::ostream &output) {
  output << "Usage: lotus-cfl-staged --grammar FILE --graph FILE --mode MODE "
            "[options]\n"
            "Modes: standard-dyck, extended-dyck, alias\n"
            "Common options:\n"
            "  --summary SYMBOL\n"
            "  --phase-l 'LHS=ATOM ATOM* ...'   Repeat for DNF alternatives\n"
            "  --phase-r 'LHS=ATOM ATOM* ...'   Repeat for DNF alternatives\n"
            "  --dump-relation [--start-only]\n"
            "  --json-stats\n"
            "Dyck options:\n"
            "  --delimiter OPEN,CLOSE            Repeat for attributed pairs\n"
            "  --neutral SYMBOL[,SYMBOL...]\n"
            "Alias-CFP options:\n"
            "  --open SYMBOL --close SYMBOL\n"
            "  --reverse-forward SYMBOL --center SYMBOL --backward SYMBOL\n";
}

std::vector<std::string> split(const std::string &text, char delimiter) {
  std::vector<std::string> result;
  std::size_t begin = 0;
  while (true) {
    const std::size_t end = text.find(delimiter, begin);
    const std::string item = text.substr(begin, end - begin);
    if (!item.empty()) {
      result.push_back(item);
    }
    if (end == std::string::npos) {
      return result;
    }
    begin = end + 1;
  }
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
    } else if (argument == "--mode") {
      const std::string selected = value();
      options.mode_selected = true;
      if (selected == "standard-dyck") {
        options.mode = Mode::StandardDyck;
      } else if (selected == "extended-dyck") {
        options.mode = Mode::ExtendedDyck;
      } else if (selected == "alias") {
        options.mode = Mode::Alias;
      } else {
        throw std::invalid_argument("Unknown staged mode: " + selected);
      }
    } else if (argument == "--summary") {
      options.summary = value();
    } else if (argument == "--delimiter") {
      options.delimiters.push_back(value());
    } else if (argument == "--neutral") {
      const auto values = split(value(), ',');
      options.neutral.insert(options.neutral.end(), values.begin(),
                             values.end());
    } else if (argument == "--open") {
      options.open = value();
    } else if (argument == "--close") {
      options.close = value();
    } else if (argument == "--reverse-forward") {
      options.reverse_forward = value();
    } else if (argument == "--center") {
      options.center = value();
    } else if (argument == "--backward") {
      options.backward = value();
    } else if (argument == "--phase-l") {
      options.phase_l.push_back(value());
    } else if (argument == "--phase-r") {
      options.phase_r.push_back(value());
    } else if (argument == "--dump-relation") {
      options.dump_relation = true;
    } else if (argument == "--start-only") {
      options.start_only = true;
    } else if (argument == "--json-stats") {
      options.json_stats = true;
    } else if (argument == "--help" || argument == "-h") {
      usage(std::cout);
      std::exit(0);
    } else {
      throw std::invalid_argument("Unknown option: " + argument);
    }
  }
  if (options.grammar.empty() || options.graph.empty() ||
      !options.mode_selected || options.summary.empty()) {
    throw std::invalid_argument(
        "--grammar, --graph, --mode, and --summary are required");
  }
  return options;
}

stg::RegularSequence parseSequence(const Grammar &grammar, std::string text) {
  stg::RegularSequence result;
  std::size_t begin = 0;
  while (begin < text.size()) {
    while (begin < text.size() && text[begin] == ' ') {
      ++begin;
    }
    if (begin == text.size()) {
      break;
    }
    const std::size_t end = text.find(' ', begin);
    std::string token = text.substr(begin, end - begin);
    begin = end == std::string::npos ? text.size() : end + 1;
    bool star = !token.empty() && token.back() == '*';
    if (star) {
      token.pop_back();
    }
    if (token.size() >= 2 && token.front() == '(' && token.back() == ')') {
      token = token.substr(1, token.size() - 2);
    }
    if (token == "epsilon" || token == "<epsilon>") {
      continue;
    }
    stg::RegularAtom atom;
    atom.kleene_star = star;
    for (const std::string &symbol : split(token, '|')) {
      atom.symbols.push_back(grammar.symbolId(symbol));
    }
    if (atom.symbols.empty()) {
      throw std::invalid_argument("Empty regular-expression atom");
    }
    result.push_back(std::move(atom));
  }
  return result;
}

std::vector<stg::RegularProduction>
parseProductions(const Grammar &grammar,
                 const std::vector<std::string> &definitions) {
  std::map<SymbolId, stg::RegularProduction> grouped;
  for (const std::string &definition : definitions) {
    const std::size_t separator = definition.find('=');
    if (separator == std::string::npos || separator == 0) {
      throw std::invalid_argument("Regular production must be LHS=EXPR");
    }
    const SymbolId lhs = grammar.symbolId(definition.substr(0, separator));
    auto [it, inserted] = grouped.emplace(lhs, stg::RegularProduction{});
    if (inserted) {
      it->second.lhs = lhs;
    }
    it->second.alternatives.push_back(
        parseSequence(grammar, definition.substr(separator + 1)));
  }
  std::vector<stg::RegularProduction> result;
  for (auto &[_, production] : grouped) {
    result.push_back(std::move(production));
  }
  return result;
}

std::vector<stg::DelimiterPair>
parseDelimiters(const Grammar &grammar,
                const std::vector<std::string> &definitions) {
  std::vector<stg::DelimiterPair> result;
  for (const std::string &definition : definitions) {
    const auto symbols = split(definition, ',');
    if (symbols.size() != 2) {
      throw std::invalid_argument("Delimiter must be OPEN,CLOSE");
    }
    result.push_back(
        {grammar.symbolId(symbols[0]), grammar.symbolId(symbols[1])});
  }
  return result;
}

std::vector<SymbolId> parseSymbols(const Grammar &grammar,
                                   const std::vector<std::string> &symbols) {
  std::vector<SymbolId> result;
  for (const std::string &symbol : symbols) {
    result.push_back(grammar.symbolId(symbol));
  }
  return result;
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    const LabeledGraph graph = LabeledGraph::parseFromFile(
        options.graph,
        GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
    const Grammar grammar =
        Grammar::parseFromFile(options.grammar, inferGrammarAttributes(graph));
    const SymbolId summary = grammar.symbolId(options.summary);
    const auto phase_l = parseProductions(grammar, options.phase_l);
    const auto phase_r = parseProductions(grammar, options.phase_r);

    stg::StagedSpecification specification;
    if (options.mode == Mode::StandardDyck ||
        options.mode == Mode::ExtendedDyck) {
      const auto delimiters = parseDelimiters(grammar, options.delimiters);
      if (delimiters.empty()) {
        throw std::invalid_argument("Dyck mode requires --delimiter");
      }
      specification =
          options.mode == Mode::StandardDyck
              ? stg::decomposeStandardDyck(
                    grammar.startSymbolId(), summary,
                    parseSymbols(grammar, options.neutral), delimiters)
              : stg::decomposeExtendedDyck(
                    grammar.startSymbolId(), summary,
                    parseSymbols(grammar, options.neutral), delimiters);
      specification.phase_l_regular.insert(specification.phase_l_regular.end(),
                                           phase_l.begin(), phase_l.end());
      specification.phase_r.insert(specification.phase_r.end(), phase_r.begin(),
                                   phase_r.end());
    } else {
      if (options.open.empty() || options.close.empty() ||
          options.reverse_forward.empty() || options.center.empty() ||
          options.backward.empty()) {
        throw std::invalid_argument(
            "Alias mode requires all five Alias-CFP symbol options");
      }
      specification =
          stg::decomposeAliasCfp({summary, grammar.symbolId(options.open),
                                  grammar.symbolId(options.close),
                                  grammar.symbolId(options.reverse_forward),
                                  grammar.symbolId(options.center),
                                  grammar.symbolId(options.backward)},
                                 phase_l, phase_r);
    }

    auto relation =
        createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
    stg::StagedSolver solver(grammar, *relation, std::move(specification),
                             graph.vertexCount());
    for (const LabeledEdge &edge : graph.edges()) {
      if (!grammar.hasSymbol(edge.label)) {
        throw std::invalid_argument("Graph label is absent from grammar: " +
                                    edge.label);
      }
      solver.addEdge(grammar.symbolId(edge.label), edge.source, edge.target);
    }
    const stg::StagedStatistics statistics = solver.solve();

    if (options.dump_relation) {
      std::vector<RelationEdge> edges = relation->edges();
      std::sort(edges.begin(), edges.end(),
                [](const auto &lhs, const auto &rhs) {
                  return std::tie(lhs.source, lhs.target, lhs.symbol) <
                         std::tie(rhs.source, rhs.target, rhs.symbol);
                });
      for (const RelationEdge &edge : edges) {
        if (options.start_only && edge.symbol != grammar.startSymbolId()) {
          continue;
        }
        std::cout << graph.vertexName(edge.source) << ','
                  << graph.vertexName(edge.target) << ','
                  << grammar.symbolName(edge.symbol) << '\n';
      }
    }
    if (options.json_stats) {
      std::cout << "{\"solver\":\"stg\",\"nodes\":" << graph.vertexCount()
                << ",\"relation_edges\":" << relation->edgeCount()
                << ",\"start_edges\":"
                << relation->edgeCount(grammar.startSymbolId())
                << ",\"phase_l_rounds\":" << statistics.phase_l_rounds
                << ",\"summary_edges\":" << statistics.summary_edges
                << ",\"phase_r_edges\":" << statistics.phase_r_edges
                << ",\"ordered_scc_propagations\":"
                << statistics.ordered_scc_propagations << "}\n";
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    usage(std::cerr);
    return 1;
  }
}
