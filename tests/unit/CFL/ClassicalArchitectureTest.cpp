#include "CFL/Classical/Solver.h"
#include "CFL/Classical/Validation.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using NamedEdge = std::tuple<std::string, std::size_t, std::size_t>;

std::set<NamedEdge> solveWith(SolverBackend backend, LabeledGraph graph,
                              const Grammar &grammar) {
  SolverSession session(graph, grammar, backend);
  session.solve();
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : session.relation().edges()) {
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  }
  return result;
}

std::filesystem::path writeTemp(const std::string &name,
                                const std::string &content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path);
  output << content;
  return path;
}

TEST(ClassicalArchitectureTest, PreservesStartAndInstantiatesAttributes) {
  const auto grammar =
      Grammar::parseFromText("Start:\n"
                             "  S\n"
                             "Terminal:\n"
                             "  call ret\n"
                             "Variables:\n"
                             "  S\n"
                             "Productions:\n"
                             "  S -> call_i S ret_i | epsilon;\n",
                             GrammarParseOptions{{3, 7}});

  EXPECT_EQ(grammar.startSymbol(), "S");
  EXPECT_TRUE(grammar.isNonterminal("S"));
  EXPECT_TRUE(grammar.isTerminal("call_3"));
  EXPECT_TRUE(grammar.isTerminal("ret_7"));
  EXPECT_FALSE(grammar.hasSymbol("call_i"));
  EXPECT_TRUE(grammar.validate().empty());
}

TEST(ClassicalArchitectureTest, CorrelatesAttributesAcrossProductionHeads) {
  const auto grammar = Grammar::parseFromText(
      "Productions:\n  Edge_i -> call_i;\n", GrammarParseOptions{{2, 5}});
  EXPECT_TRUE(grammar.isNonterminal("Edge_2"));
  EXPECT_TRUE(grammar.isNonterminal("Edge_5"));
  EXPECT_NE(grammar.unaryByRhs().find("call_2"), grammar.unaryByRhs().end());
  EXPECT_NE(grammar.unaryByRhs().find("call_5"), grammar.unaryByRhs().end());
}

TEST(ClassicalArchitectureTest, AllSolverBackendsProduceTheSameClosure) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a | epsilon;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");

  const auto baseline = solveWith(SolverBackend::Baseline, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::POCR, graph, grammar), baseline);
  EXPECT_EQ(solveWith(SolverBackend::Hybrid, graph, grammar), baseline);
  EXPECT_TRUE(baseline.count({"S", 0, 3}) != 0);
}

TEST(ClassicalArchitectureTest, BackendsAgreeAcrossGeneratedSmallGraphs) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> a; B -> b; S -> S S | A B | epsilon;\n");

  for (std::size_t seed = 1; seed <= 24; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 6; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 6; ++source) {
      for (std::size_t target = 0; target < 6; ++target) {
        const std::size_t value = source * 17 + target * 31 + seed * 13;
        if (value % 11 == 0) {
          graph.addEdge(source, target, "a");
        }
        if (value % 13 == 0) {
          graph.addEdge(source, target, "b");
        }
      }
    }

    const auto baseline = solveWith(SolverBackend::Baseline, graph, grammar);
    EXPECT_EQ(solveWith(SolverBackend::POCR, graph, grammar), baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::Hybrid, graph, grammar), baseline)
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, IncrementalSessionResumesToFixedPoint) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S\nProductions:\n"
      "  S -> a b;\n");
  LabeledGraph graph;
  const auto n0 = graph.addVertex("n0");
  const auto n1 = graph.addVertex("n1");
  const auto n2 = graph.addVertex("n2");
  graph.addEdge(n0, n1, "a");

  SolverSession session(graph, grammar, SolverBackend::POCR);
  session.solve();
  EXPECT_FALSE(session.contains(n0, n2, "S"));
  EXPECT_FALSE(graph.hasEdge(n0, n2, "S"));

  EXPECT_TRUE(session.addTerminalEdge(n1, n2, "b"));
  const auto stats = session.solve();
  EXPECT_TRUE(session.contains(n0, n2, "S"));
  EXPECT_FALSE(graph.hasEdge(n0, n2, "S"));
  EXPECT_GT(stats.added_edges, 0u);
}

TEST(ClassicalArchitectureTest, IncrementalSessionSupportsDiscoveredNodes) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> a | epsilon;\n");
  LabeledGraph graph;
  graph.addVertex("existing");
  SolverSession session(graph, grammar, SolverBackend::POCR);
  session.solve();

  const auto discovered = session.addNode("discovered");
  session.solve();
  EXPECT_TRUE(session.contains(discovered, discovered, "S"));
}

TEST(ClassicalArchitectureTest, DirectionTransformsAreExplicitAndAttributed) {
  LabeledGraph graph;
  graph.addEdge("caller", "callee", "call_9");
  const auto transformed = graph.transformed(EdgeDirection::Bidirectional);
  EXPECT_TRUE(transformed.hasEdge(0, 1, "call_9"));
  EXPECT_TRUE(transformed.hasEdge(1, 0, "callbar_9"));
  EXPECT_EQ(LabeledGraph::complementLabel("callbar_9"), "call_9");
}

TEST(ClassicalArchitectureTest, BuildsFromClientOwnedRanges) {
  struct Edge {
    std::string source;
    std::string target;
    std::string label;
  };
  const std::vector<std::string> nodes{"left", "right"};
  const std::vector<Edge> edges{{"left", "right", "a"}};
  const auto graph = LabeledGraph::build(
      nodes, edges, [](const std::string &node) { return node; },
      [](const Edge &edge) { return edge.source; },
      [](const Edge &edge) { return edge.target; },
      [](const Edge &edge) { return edge.label; });
  EXPECT_TRUE(graph.hasEdge(0, 1, "a"));
}

TEST(ClassicalArchitectureTest, LoadsJsonGraphAndValidatesLabels) {
  const auto path =
      writeTemp("lotus_classical_graph.json",
                R"({"edges":[{"source":"n0","target":"n1","label":"a"}]})");
  const auto graph = LabeledGraph::parseFromFile(
      path.string(), GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> a;\n");

  EXPECT_TRUE(graph.hasEdge(0, 1, "a"));
  EXPECT_TRUE(validateGraph(graph, grammar).empty());

  LabeledGraph invalid_graph = graph;
  invalid_graph.addEdge(0, 1, "unknown");
  const auto issues = validateGraph(invalid_graph, grammar);
  EXPECT_TRUE(
      std::any_of(issues.begin(), issues.end(), [](const GrammarIssue &issue) {
        return issue.severity == GrammarIssueSeverity::Error;
      }));
}

} // namespace
} // namespace lotus::cfl::classical
