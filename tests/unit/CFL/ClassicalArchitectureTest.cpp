#include "CFL/Classical/Core/Validation.h"
#include "CFL/Classical/Solvers/Reachability.h"
#include "CFL/Classical/Solvers/TransitiveClosure.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
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

std::set<NamedEdge> solveReference(const LabeledGraph &graph,
                                   const Grammar &grammar) {
  const std::size_t node_count = graph.vertexCount();
  std::vector<std::vector<std::vector<bool>>> relation(
      grammar.symbolCount(),
      std::vector<std::vector<bool>>(node_count,
                                     std::vector<bool>(node_count, false)));
  for (const LabeledEdge &edge : graph.edges()) {
    relation[grammar.symbolId(edge.label)][edge.source][edge.target] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[head, rules] : grammar.productions()) {
      const SymbolId lhs = grammar.symbolId(head);
      for (const auto &rule : rules) {
        if (rule.size() == 1 && rule.front() == Grammar::kEpsilonSymbol) {
          for (std::size_t node = 0; node < node_count; ++node) {
            if (!relation[lhs][node][node]) {
              relation[lhs][node][node] = true;
              changed = true;
            }
          }
        } else if (rule.size() == 1) {
          const SymbolId rhs = grammar.symbolId(rule.front());
          for (std::size_t source = 0; source < node_count; ++source) {
            for (std::size_t target = 0; target < node_count; ++target) {
              if (relation[rhs][source][target] &&
                  !relation[lhs][source][target]) {
                relation[lhs][source][target] = true;
                changed = true;
              }
            }
          }
        } else if (rule.size() == 2) {
          const SymbolId first = grammar.symbolId(rule[0]);
          const SymbolId second = grammar.symbolId(rule[1]);
          for (std::size_t source = 0; source < node_count; ++source) {
            for (std::size_t middle = 0; middle < node_count; ++middle) {
              if (!relation[first][source][middle]) {
                continue;
              }
              for (std::size_t target = 0; target < node_count; ++target) {
                if (relation[second][middle][target] &&
                    !relation[lhs][source][target]) {
                  relation[lhs][source][target] = true;
                  changed = true;
                }
              }
            }
          }
        }
      }
    }
  }

  std::set<NamedEdge> result;
  for (SymbolId symbol = 0; symbol < grammar.symbolCount(); ++symbol) {
    for (std::size_t source = 0; source < node_count; ++source) {
      for (std::size_t target = 0; target < node_count; ++target) {
        if (relation[symbol][source][target]) {
          result.emplace(grammar.symbolName(symbol), source, target);
        }
      }
    }
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
  GrammarParseOptions options;
  options.variable_attributes['i'] = {3, 7};
  const auto grammar =
      Grammar::parseFromText("Start:\n"
                             "  S\n"
                             "Terminal:\n"
                             "  call ret\n"
                             "Variables:\n"
                             "  S\n"
                             "Productions:\n"
                             "  S -> call_i S ret_i | <epsilon>;\n",
                             options);

  EXPECT_EQ(grammar.startSymbol(), "S");
  EXPECT_TRUE(grammar.isNonterminal("S"));
  EXPECT_TRUE(grammar.isTerminal("call_3"));
  EXPECT_TRUE(grammar.isTerminal("ret_7"));
  EXPECT_FALSE(grammar.hasSymbol("call_i"));
  EXPECT_TRUE(grammar.validate().empty());
}

TEST(ClassicalArchitectureTest,
     GeneratedNonterminalsAreNeverClassifiedAsTerminals) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a __lotus_generated_1\n"
                             "Variables:\n  S\nProductions:\n"
                             "  S -> ( a ? ) * a a a;\n");

  for (const std::string &nonterminal : grammar.nonterminals()) {
    EXPECT_FALSE(grammar.isTerminal(nonterminal)) << nonterminal;
  }
  EXPECT_TRUE(grammar.validate().empty());
}

TEST(ClassicalArchitectureTest, ValidationRejectsSymbolKindIntersection) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  S a\nVariables:\n  S\n"
                             "Productions:\n  S -> a;\n");
  const auto issues = grammar.validate();
  EXPECT_TRUE(
      std::any_of(issues.begin(), issues.end(), [](const GrammarIssue &issue) {
        return issue.severity == GrammarIssueSeverity::Error &&
               issue.message.find("both terminal and nonterminal") !=
                   std::string::npos;
      }));
}

TEST(ClassicalArchitectureTest, CorrelatesAttributesAcrossProductionHeads) {
  GrammarParseOptions options;
  options.variable_attributes['i'] = {2, 5};
  const auto grammar =
      Grammar::parseFromText("Productions:\n  Edge_i -> call_i;\n", options);
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

TEST(ClassicalArchitectureTest, RejectsExplosiveAttributeExpansion) {
  GrammarParseOptions options;
  options.variable_attributes['i'] = {1, 2, 3};
  options.variable_attributes['j'] = {4, 5, 6};
  options.max_attribute_expansions = 8;
  EXPECT_THROW(
      Grammar::parseFromText(
          "Productions:\n  S -> call_i ret_i load_j store_j;\n", options),
      std::length_error);
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

TEST(ClassicalArchitectureTest, RejectsOutOfRangeGraphAttributes) {
  LabeledGraph graph;
  graph.addEdge("a", "b", "call_4294967296");
  EXPECT_THROW(inferGrammarAttributes(graph), std::invalid_argument);
}

TEST(ClassicalArchitectureTest,
     SparseBitvectorsMaintainIncrementalTransitiveClosure) {
  IncrementalTransitiveClosure closure(5);
  EXPECT_EQ(closure.addArc(0, 1).size(), 1u);
  const auto through_two = closure.addArc(1, 2);
  EXPECT_NE(std::find(through_two.begin(), through_two.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 2)),
            through_two.end());

  const auto prefixed = closure.addArc(3, 0);
  EXPECT_NE(std::find(prefixed.begin(), prefixed.end(),
                      std::make_pair<std::size_t, std::size_t>(3, 2)),
            prefixed.end());

  const auto cycle = closure.addArc(2, 0);
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 0)),
            cycle.end());
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(2, 1)),
            cycle.end());
  EXPECT_TRUE(closure.hasPath(0, 0));
  EXPECT_GT(closure.statistics().propagated_pairs, 0u);

  closure.ensureNodeCount(6);
  EXPECT_EQ(closure.addArc(5, 0).size(), 3u);
}

TEST(ClassicalArchitectureTest, AllSolverBackendsProduceTheSameClosure) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a | <epsilon>;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");

  const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::SparseBitVector, graph, grammar),
            baseline);
  EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
            baseline);
  EXPECT_TRUE(baseline.count({"S", 0, 3}) != 0);

  LabeledGraph transitive_graph = graph;
  SolverSession transitive(transitive_graph, grammar,
                           SolverBackend::TransitiveClosure);
  const ReachabilityStats transitive_stats = transitive.solve();
  EXPECT_EQ(transitive_stats.transitive_closure_instances, 1u);
  EXPECT_GT(transitive_stats.transitive_relation_edges, graph.edgeCount());
  EXPECT_GT(transitive_stats.transitive_propagated_pairs, 0u);
}

TEST(ClassicalArchitectureTest, BackendsAgreeAcrossGeneratedSmallGraphs) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> a; B -> b; S -> S S | A B | <epsilon>;\n");

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

    const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);
    EXPECT_EQ(solveWith(SolverBackend::SparseBitVector, graph, grammar),
              baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
              baseline)
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest,
     AllBackendsMatchCubicReferenceAcrossRandomProblems) {
  for (std::uint64_t seed = 1; seed <= 1000; ++seed) {
    std::uint64_t state = seed;
    auto next = [&]() {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return state;
    };

    std::ostringstream grammar_text;
    grammar_text << "Start:\n  S\nTerminal:\n  a b c\nVariables:\n  S A B C\n"
                    "Productions:\n  A -> a";
    if ((next() & 1U) != 0) {
      grammar_text << " | <epsilon>";
    }
    grammar_text << ";\n  B -> b";
    if ((next() & 1U) != 0) {
      grammar_text << " | A";
    }
    grammar_text << ";\n  C -> c";
    if ((next() & 1U) != 0) {
      grammar_text << " | A B";
    }
    grammar_text << ";\n  S -> A B";
    if ((next() & 1U) != 0) {
      grammar_text << " | B C";
    }
    if ((next() & 1U) != 0) {
      grammar_text << " | C A";
    }
    if ((next() & 1U) != 0) {
      grammar_text << " | S S";
    }
    if ((next() & 1U) != 0) {
      grammar_text << " | <epsilon>";
    }
    grammar_text << ";\n";
    const Grammar grammar = Grammar::parseFromText(grammar_text.str());

    LabeledGraph graph;
    for (std::size_t node = 0; node < 4; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 4; ++source) {
      for (std::size_t target = 0; target < 4; ++target) {
        for (const std::string &label : {"a", "b", "c"}) {
          if (next() % 7 == 0) {
            graph.addEdge(source, target, label);
          }
        }
      }
    }

    const auto reference = solveReference(graph, grammar);
    for (SolverBackend backend :
         {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
          SolverBackend::TransitiveClosure}) {
      EXPECT_EQ(solveWith(backend, graph, grammar), reference)
          << "seed=" << seed << " backend=" << solverBackendName(backend);
    }
  }
}

TEST(ClassicalArchitectureTest,
     TransitiveClosureDerivesNonNullableReflexiveCycles) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n0", "a");

  const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
            baseline);
  EXPECT_TRUE(baseline.count({"S", 0, 0}) != 0);
  EXPECT_TRUE(baseline.count({"S", 1, 1}) != 0);
}

TEST(ClassicalArchitectureTest,
     TransitiveClosureMatchesSparseSetOnGeneratedCyclicGraphs) {
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
    EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
              solveWith(SolverBackend::SparseSet, graph, grammar))
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, UsesOneClosurePerTransitiveGrammarSymbol) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> A A | a; B -> B B | b; S -> A B;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "b");
  graph.addEdge("n3", "n4", "b");

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  SolverSession transitive(graph, grammar, SolverBackend::TransitiveClosure);
  const ReachabilityStats stats = transitive.solve();

  EXPECT_TRUE(transitive.contains(0, 4, "S"));
  EXPECT_EQ(stats.transitive_closure_instances, 2u);
  EXPECT_EQ(transitive.relation().edgeCount(), baseline.relation().edgeCount());
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

  SolverSession session(graph, grammar, SolverBackend::SparseBitVector);
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
      "  S -> a | <epsilon>;\n");
  LabeledGraph graph;
  graph.addVertex("existing");
  SolverSession session(graph, grammar, SolverBackend::SparseBitVector);
  session.solve();

  const auto discovered = session.addNode("discovered");
  session.solve();
  EXPECT_TRUE(session.contains(discovered, discovered, "S"));
}

TEST(ClassicalArchitectureTest, DetectsGraphMutationOutsideSession) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addVertex("n2");
  SolverSession session(graph, grammar);
  session.solve();

  graph.addEdge(1, 2, "a");
  EXPECT_THROW(session.solve(), std::logic_error);
  EXPECT_THROW(session.contains(1, 2, "S"), std::logic_error);
}

TEST(ClassicalArchitectureTest, RelationSupportsPerSymbolQueriesAndCounts) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> S S | a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  SolverSession session(graph, grammar, SolverBackend::TransitiveClosure);
  session.solve();

  const SymbolId symbol = grammar.symbolId("S");
  EXPECT_EQ(session.relation().edgeCount(symbol),
            session.relation().edges(symbol).size());
  std::size_t successor_count = 0;
  session.relation().forEachSuccessor(symbol, 0,
                                      [&](NodeId) { ++successor_count; });
  EXPECT_EQ(successor_count, 2u);
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

TEST(ClassicalArchitectureTest,
     JsonAndDotInputsPreserveIsolatedAndQuotedVertices) {
  const auto json_path =
      writeTemp("lotus_classical_isolated.json",
                R"({"nodes":["isolated",{"id":"connected node"}],"edges":[]})");
  const LabeledGraph json = LabeledGraph::parseFromFile(
      json_path.string(),
      GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  EXPECT_EQ(json.vertexCount(), 2u);
  EXPECT_EQ(json.vertexName(0), "isolated");
  const auto nullable = Grammar::parseFromText(
      "Start:\n  S\nVariables:\n  S\nProductions:\n  S -> <epsilon>;\n");
  LabeledGraph nullable_graph = json;
  SolverSession nullable_session(nullable_graph, nullable);
  nullable_session.solve();
  EXPECT_TRUE(nullable_session.contains(0, 0, "S"));
  EXPECT_TRUE(nullable_session.contains(1, 1, "S"));

  const auto dot_path =
      writeTemp("lotus_classical_quoted.dot",
                "digraph G {\n  \"isolated node\";\n  \"source.node\" -> "
                "\"target-node\" [label=\"edge.label\"];\n}\n");
  const LabeledGraph dot = LabeledGraph::parseFromFile(
      dot_path.string(),
      GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  EXPECT_EQ(dot.vertexCount(), 3u);
  EXPECT_TRUE(dot.hasEdge(dot.vertexId("source.node"),
                          dot.vertexId("target-node"), "edge.label"));
}

} // namespace
} // namespace lotus::cfl::classical
