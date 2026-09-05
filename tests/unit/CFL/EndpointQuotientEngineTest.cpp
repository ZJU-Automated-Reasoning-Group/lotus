#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/Engines/EndpointQuotient/EndpointQuotient.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using endpoint::Id;
using endpoint::Problem;
using endpoint::Rule;
using endpoint::Solver;
using NamedEdge = std::tuple<std::string, std::size_t, std::size_t>;

const Grammar &starGrammar() {
  static const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a | <epsilon>;\n");
  return grammar;
}

std::set<NamedEdge> sessionEdges(SolverSession &session,
                                 const Grammar &grammar) {
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : session.relation().edges()) {
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  }
  return result;
}

std::set<NamedEdge> referenceClosure(const LabeledGraph &graph,
                                     const Grammar &grammar) {
  const std::size_t node_count = graph.vertexCount();
  const std::size_t symbol_count = grammar.symbolCount();
  std::vector<std::vector<std::vector<bool>>> relation(
      symbol_count, std::vector<std::vector<bool>>(
                        node_count, std::vector<bool>(node_count, false)));
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
              for (std::size_t target = 0; target < node_count; ++target) {
                if (relation[first][source][middle] &&
                    relation[second][middle][target] &&
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
  for (std::size_t symbol = 0; symbol < symbol_count; ++symbol) {
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

} // namespace

TEST(EndpointQuotientCoreTest, ArtifactExampleBinaryRule) {
  Problem problem{5,
                  3,
                  {{0, 0, 2}, {1, 0, 2}, {2, 1, 3}, {2, 1, 4}},
                  {Rule::binary(2, 0, 1)}};
  Solver solver(std::move(problem));
  solver.solve();
  EXPECT_TRUE(solver.contains(0, 0, 2));
  EXPECT_TRUE(solver.contains(2, 0, 4));
  EXPECT_TRUE(solver.contains(2, 1, 4));
  EXPECT_FALSE(solver.contains(2, 0, 2));
  EXPECT_FALSE(solver.contains(2, 1, 2));
  EXPECT_EQ(solver.statistics().cells, 3u);
  EXPECT_EQ(solver.statistics().logical_facts, 8u);
}

TEST(EndpointQuotientCoreTest, NullableDiagonalStaysSymbolic) {
  Problem problem{3, 2, {{0, 0, 1}}, {Rule::epsilon(1), Rule::unary(1, 0)}};
  Solver solver(std::move(problem));
  solver.solve();
  EXPECT_TRUE(solver.isNullable(1));
  EXPECT_TRUE(solver.contains(1, 0, 0));
  EXPECT_TRUE(solver.contains(1, 1, 1));
  EXPECT_TRUE(solver.contains(1, 2, 2));
  EXPECT_TRUE(solver.contains(1, 0, 1));
  EXPECT_FALSE(solver.contains(1, 0, 2));
}

TEST(EndpointQuotientSessionTest, MatchesReferenceClosure) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");

  const auto expected = referenceClosure(graph, grammar);
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  session.solve();
  EXPECT_EQ(sessionEdges(session, grammar), expected);
  EXPECT_TRUE(session.contains(graph.vertexId("n0"), graph.vertexId("n3"),
                               "S"));
}

TEST(EndpointQuotientSessionTest, StarGraphCompresses) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  const std::size_t leaves = 100;
  for (std::size_t leaf = 1; leaf <= leaves; ++leaf) {
    graph.addEdge("hub", "leaf" + std::to_string(leaf), "a");
    graph.addEdge("leaf" + std::to_string(leaf), "hub", "a");
  }

  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  const ReachabilityStats stats = session.solve();
  EXPECT_EQ(sessionEdges(session, grammar), referenceClosure(graph, grammar));
  EXPECT_EQ(stats.endpoint_quotient_cells, 6u);
  EXPECT_GT(stats.endpoint_quotient_facts,
            static_cast<std::size_t>(leaves * leaves));
}

TEST(EndpointQuotientSessionTest, MatchesSparseSetBackend) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");
  graph.addEdge("n3", "n4", "a");

  SolverSession baseline(graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  SolverSession quotient(graph, grammar, SolverBackend::EndpointQuotient);
  quotient.solve();
  EXPECT_EQ(sessionEdges(quotient, grammar),
            sessionEdges(baseline, grammar));
}

TEST(EndpointQuotientSessionTest, IncrementalTerminalEdgesResolve) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addVertex("n2");
  graph.addEdge("n0", "n1", "a");

  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  session.solve();
  EXPECT_TRUE(session.contains(graph.vertexId("n0"), graph.vertexId("n1"),
                               "S"));
  EXPECT_FALSE(session.contains(graph.vertexId("n0"), graph.vertexId("n2"),
                                "S"));

  session.addTerminalEdge(graph.vertexId("n1"), graph.vertexId("n2"), "a");
  session.solve();
  EXPECT_TRUE(session.contains(graph.vertexId("n0"), graph.vertexId("n2"),
                               "S"));
  EXPECT_TRUE(session.contains(graph.vertexId("n0"), graph.vertexId("n1"),
                               "S"));
}

TEST(EndpointQuotientSessionTest, EmptyGraphWithNullableGrammar) {
  const Grammar &grammar = starGrammar();
  LabeledGraph graph;
  graph.addVertex("n0");
  SolverSession session(graph, grammar, SolverBackend::EndpointQuotient);
  session.solve();
  EXPECT_TRUE(session.contains(0, 0, "S"));
}

} // namespace lotus::cfl::classical