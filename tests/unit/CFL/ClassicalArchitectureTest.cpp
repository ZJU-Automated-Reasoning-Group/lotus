#include "CFL/Classical/HybridForest.h"
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

TEST(ClassicalArchitectureTest, IntersectsPerKindAttributeDomains) {
  GrammarParseOptions options;
  options.symbol_attributes["call"] = {1, 2};
  options.symbol_attributes["ret"] = {2, 3};
  const auto grammar =
      Grammar::parseFromText("Productions:\n  S -> call_i ret_i;\n", options);

  EXPECT_TRUE(grammar.hasSymbol("call_2"));
  EXPECT_TRUE(grammar.hasSymbol("ret_2"));
  EXPECT_TRUE(grammar.isTerminal("call_1"));
  EXPECT_TRUE(grammar.isTerminal("ret_3"));
  EXPECT_EQ(grammar.binaryByFirst().count("call_1"), 0u);
  EXPECT_EQ(grammar.binaryBySecond().count("ret_3"), 0u);
}

TEST(ClassicalArchitectureTest, RejectsUnboundAttributeVariables) {
  EXPECT_THROW(Grammar::parseFromText("Productions:\n  S -> call_i ret_i;\n"),
               std::invalid_argument);
}

TEST(ClassicalArchitectureTest, InfersAttributeDomainsFromGraphLabels) {
  LabeledGraph graph;
  graph.addEdge("a", "b", "call_4");
  graph.addEdge("b", "c", "ret_4");
  graph.addEdge("c", "d", "call_9");
  graph.addEdge("d", "e", "ret_9");
  const auto grammar = Grammar::parseFromText(
      "Productions:\n  S -> call_i ret_i;\n", inferGrammarAttributes(graph));

  EXPECT_TRUE(grammar.hasSymbol("call_4"));
  EXPECT_TRUE(grammar.hasSymbol("call_9"));
  EXPECT_TRUE(grammar.hasSymbol("ret_4"));
  EXPECT_TRUE(grammar.hasSymbol("ret_9"));
}

TEST(ClassicalArchitectureTest, HybridForestMaintainsIncrementalReachTrees) {
  HybridReachabilityForest forest(5);
  EXPECT_EQ(forest.addArc(0, 1).size(), 1u);
  const auto through_two = forest.addArc(1, 2);
  EXPECT_NE(std::find(through_two.begin(), through_two.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 2)),
            through_two.end());

  const auto prefixed = forest.addArc(3, 0);
  EXPECT_NE(std::find(prefixed.begin(), prefixed.end(),
                      std::make_pair<std::size_t, std::size_t>(3, 2)),
            prefixed.end());

  const auto cycle = forest.addArc(2, 0);
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 0)),
            cycle.end());
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(2, 1)),
            cycle.end());
  EXPECT_GT(forest.statistics().meld_operations, 0u);

  forest.ensureNodeCount(6);
  EXPECT_EQ(forest.addArc(5, 0).size(), 3u);
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

  LabeledGraph hybrid_graph = graph;
  SolverSession hybrid(hybrid_graph, grammar, SolverBackend::Hybrid);
  const ReachabilityStats hybrid_stats = hybrid.solve();
  EXPECT_EQ(hybrid_stats.hybrid_forest_roots, graph.vertexCount());
  EXPECT_GT(hybrid_stats.hybrid_forest_nodes, hybrid_stats.hybrid_forest_roots);
  EXPECT_GT(hybrid_stats.hybrid_meld_operations, 0u);
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

TEST(ClassicalArchitectureTest, HybridForestDerivesNonNullableReflexiveCycles) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n0", "a");

  const auto baseline = solveWith(SolverBackend::Baseline, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::Hybrid, graph, grammar), baseline);
  EXPECT_TRUE(baseline.count({"S", 0, 0}) != 0);
  EXPECT_TRUE(baseline.count({"S", 1, 1}) != 0);
}

TEST(ClassicalArchitectureTest,
     HybridForestMatchesBaselineOnGeneratedCyclicGraphs) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a;\n");
  for (std::size_t seed = 1; seed <= 32; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 7; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 7; ++source) {
      for (std::size_t target = 0; target < 7; ++target) {
        if ((source * 19 + target * 23 + seed * 29) % 9 == 0) {
          graph.addEdge(source, target, "a");
        }
      }
    }
    EXPECT_EQ(solveWith(SolverBackend::Hybrid, graph, grammar),
              solveWith(SolverBackend::Baseline, graph, grammar))
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, HybridUsesOneForestPerTransitiveSymbol) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> A A | a; B -> B B | b; S -> A B;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "b");
  graph.addEdge("n3", "n4", "b");

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::Baseline);
  baseline.solve();
  SolverSession hybrid(graph, grammar, SolverBackend::Hybrid);
  const ReachabilityStats stats = hybrid.solve();

  EXPECT_TRUE(hybrid.contains(0, 4, "S"));
  EXPECT_EQ(stats.hybrid_forest_roots, graph.vertexCount() * 2);
  EXPECT_EQ(hybrid.relation().edgeCount(), baseline.relation().edgeCount());
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
