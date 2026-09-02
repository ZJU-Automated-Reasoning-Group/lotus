#include "CFL/Classical/Core/Grammar.h"
#include "CFL/Classical/Core/Graph.h"
#include "CFL/Classical/Solvers/ConstraintGrounding.h"
#include "CFL/Classical/Solvers/Reachability.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

std::filesystem::path createTempFile(const std::string &name,
                                     const std::string &content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path);
  output << content;
  return path;
}

} // namespace

TEST(ClassicalGrammarTest, ParsesAndNormalizesDemoGrammar) {
  const auto grammar_path =
      createTempFile("lotus_classical_grammar.txt",
                     "Terminals:\n"
                     "  dbar d abar a\n"
                     "Variables:\n"
                     "  M V\n"
                     "Start:\n"
                     "  M\n"
                     "Productions:\n"
                     "  M -> dbar V d;\n"
                     "  V -> ( M ? abar ) * M ? ( a M ? ) *;\n");

  const auto grammar = Grammar::parseFromFile(grammar_path.string());

  EXPECT_FALSE(grammar.productions().empty());
  EXPECT_FALSE(grammar.nullableSymbols().empty());
  EXPECT_NE(grammar.binaryByFirst().find("dbar"),
            grammar.binaryByFirst().end());
}

TEST(ClassicalGraphTest, ParsesDotGraphWithMatrixAndPagModes) {
  const auto graph_path = createTempFile("lotus_classical_graph.dot",
                                         "digraph \"PEG\" {\n"
                                         "  NodeA [shape=circle];\n"
                                         "  NodeB [shape=circle];\n"
                                         "  NodeA -> NodeB[color=black];\n"
                                         "  NodeB -> NodeA[color=red];\n"
                                         "}\n");

  const auto matrix_graph =
      LabeledGraph::parseFromFile(graph_path.string(), GraphMode::Matrix);
  EXPECT_TRUE(matrix_graph.hasEdge(matrix_graph.vertexId("NodeA"),
                                   matrix_graph.vertexId("NodeB"), "a"));
  EXPECT_TRUE(matrix_graph.hasEdge(matrix_graph.vertexId("NodeB"),
                                   matrix_graph.vertexId("NodeA"), "abar"));

  const auto pag_graph =
      LabeledGraph::parseFromFile(graph_path.string(), GraphMode::PAGMatrix);
  EXPECT_TRUE(pag_graph.hasEdge(pag_graph.vertexId("NodeB"),
                                pag_graph.vertexId("NodeA"), "d"));
  EXPECT_TRUE(pag_graph.hasEdge(pag_graph.vertexId("NodeA"),
                                pag_graph.vertexId("NodeB"), "dbar"));
}

TEST(ClassicalGraphTest, RejectsInvalidEndpointsWithoutMutation) {
  LabeledGraph graph;
  graph.addVertex("only");
  const std::uint64_t version = graph.mutationVersion();

  EXPECT_THROW(graph.addEdge(0, 100000, "a"), std::out_of_range);
  EXPECT_EQ(graph.edgeCount(), 0u);
  EXPECT_EQ(graph.mutationVersion(), version);
}

TEST(ClassicalSolverTest, DerivesReachableLabels) {
  auto graph = LabeledGraph{};
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");

  const auto grammar_path =
      createTempFile("lotus_classical_solver_grammar.txt", "Terminals:\n"
                                                           "  a b\n"
                                                           "Variables:\n"
                                                           "  S A B\n"
                                                           "Start:\n"
                                                           "  S\n"
                                                           "Productions:\n"
                                                           "  A -> a;\n"
                                                           "  B -> b;\n"
                                                           "  S -> A B;\n");

  const auto grammar = Grammar::parseFromFile(grammar_path.string());
  SolverSession session(graph, grammar);
  const auto stats = session.solve();

  EXPECT_TRUE(
      session.contains(graph.vertexId("n0"), graph.vertexId("n2"), "S"));
  EXPECT_FALSE(graph.hasEdge(graph.vertexId("n0"), graph.vertexId("n2"), "S"));
  EXPECT_GT(stats.classical_iterations, 0U);
}

TEST(ClassicalConstraintGroundingTest, ProducesConstraintStatistics) {
  auto graph = LabeledGraph{};
  graph.addEdge("n0", "n1", "a");

  const auto grammar_path =
      createTempFile("lotus_classical_sc_grammar.txt", "Terminals:\n"
                                                       "  a\n"
                                                       "Variables:\n"
                                                       "  A\n"
                                                       "Start:\n"
                                                       "  A\n"
                                                       "Productions:\n"
                                                       "  A -> a;\n");

  const auto grammar = Grammar::parseFromFile(grammar_path.string());
  const ConstraintGroundingSolver solver;
  const auto stats = solver.ground(graph, grammar);

  EXPECT_GT(stats.constraint_variables, 0U);
  EXPECT_GT(stats.set_variables, 0U);
  EXPECT_GT(stats.processed_constraints, 0U);
}

TEST(ClassicalConstraintGroundingTest, VisitsOnlyIndexedConstraintBuckets) {
  LabeledGraph graph;
  for (std::size_t node = 0; node < 128; ++node) {
    graph.addVertex("n" + std::to_string(node));
  }
  for (std::size_t node = 0; node + 1 < 128; ++node) {
    graph.addEdge(node, node + 1, "a");
  }
  const auto grammar =
      Grammar::parseFromText("Start:\n  A\nTerminal:\n  a\nVariables:\n  A\n"
                             "Productions:\n  A -> a;\n");

  const ConstraintGroundingStatistics stats =
      ConstraintGroundingSolver().ground(graph, grammar);
  EXPECT_LT(stats.processed_constraints, 1024u);
  EXPECT_EQ(stats.grounded_variables, stats.set_variables);
}

TEST(ClassicalGrammarTest, TerminalUseNeverReusesAProducingNonterminal) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b c\nVariables:\n  S A C\n"
      "Productions:\n  S -> a C; A -> a | b; C -> c;\n");

  LabeledGraph accepted;
  accepted.addEdge("n0", "n1", "a");
  accepted.addEdge("n1", "n2", "c");
  SolverSession accepted_session(accepted, grammar);
  accepted_session.solve();
  EXPECT_TRUE(accepted_session.contains(0, 2, "S"));

  LabeledGraph rejected;
  rejected.addEdge("n0", "n1", "b");
  rejected.addEdge("n1", "n2", "c");
  SolverSession rejected_session(rejected, grammar);
  rejected_session.solve();
  EXPECT_FALSE(rejected_session.contains(0, 2, "S"));
}

TEST(ClassicalGrammarTest, SingleLetterETerminalIsNotEpsilon) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  e\nVariables:\n  S\n"
                             "Productions:\n  S -> e;\n");
  EXPECT_TRUE(grammar.isTerminal("e"));
  EXPECT_TRUE(grammar.nullableSymbols().empty());

  LabeledGraph graph;
  graph.addEdge("n0", "n1", "e");
  SolverSession session(graph, grammar);
  session.solve();
  EXPECT_TRUE(session.contains(0, 1, "S"));
  EXPECT_FALSE(session.contains(0, 0, "S"));
}

} // namespace lotus::cfl::classical
