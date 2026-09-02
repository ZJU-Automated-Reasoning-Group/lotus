#include "CFL/Classical/CNF.h"
#include "CFL/Classical/Grammar.h"
#include "CFL/Classical/Graph.h"
#include "CFL/Classical/SCSolver.h"
#include "CFL/Classical/Solver.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

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

TEST(ClassicalSCSolverTest, ProducesConstraintStatistics) {
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
  const SCSolver solver;
  const auto stats = solver.solve(graph, grammar);

  EXPECT_GT(stats.constraint_variables, 0U);
  EXPECT_GT(stats.set_variables, 0U);
  EXPECT_GT(stats.classical_iterations, 0U);
}

TEST(ClassicalSCSolverTest, VisitsOnlyIndexedConstraintBuckets) {
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

  const SCStatistics stats = SCSolver().solve(graph, grammar);
  EXPECT_LT(stats.classical_iterations, 1024u);
  EXPECT_EQ(stats.grounded_variables, stats.set_variables);
}

TEST(ClassicalCNFTest, AppliesStbduPipeline) {
  const auto grammar_path =
      createTempFile("lotus_classical_cnf.txt", "Terminals:\n"
                                                "  a b\n"
                                                "Variables:\n"
                                                "  S A\n"
                                                "Productions:\n"
                                                "  S -> A b a;\n"
                                                "  A -> a | e;\n");

  const auto grammar = CNFGrammar::transformToSTBDU(grammar_path.string());

  EXPECT_FALSE(grammar.productions().empty());
  EXPECT_TRUE(
      std::any_of(grammar.productions().begin(), grammar.productions().end(),
                  [](const CNFRule &rule) { return rule.lhs == "S0"; }));
  EXPECT_TRUE(
      std::all_of(grammar.productions().begin(), grammar.productions().end(),
                  [](const CNFRule &rule) { return !rule.rhs.empty(); }));
}

TEST(ClassicalCNFTest, PreservesNullableStartLanguage) {
  const auto grammar_path =
      createTempFile("lotus_classical_cnf_epsilon.txt",
                     "Terminals:\n  a\nVariables:\n  S A B\nProductions:\n"
                     "  S -> A B; A -> e; B -> e;\n");
  const auto grammar = CNFGrammar::transformToSTBDU(grammar_path.string());

  EXPECT_NE(std::find(grammar.productions().begin(),
                      grammar.productions().end(), CNFRule{"S0", {"e"}}),
            grammar.productions().end());
}

TEST(ClassicalCNFTest, EliminatesUnitChainsWithoutIterationLimit) {
  constexpr std::size_t chain_length = 1100;
  std::ostringstream text;
  text << "Terminals:\n  a\nVariables:\n  ";
  for (std::size_t i = 0; i <= chain_length; ++i) {
    text << "A" << i << ' ';
  }
  text << "\nProductions:\n";
  for (std::size_t i = 0; i < chain_length; ++i) {
    text << "A" << i << " -> A" << i + 1 << ";\n";
  }
  text << "A" << chain_length << " -> a;\n";
  const auto path =
      createTempFile("lotus_classical_cnf_unit_chain.txt", text.str());

  const auto grammar = CNFGrammar::transformToSTBDU(path.string());
  EXPECT_NE(std::find(grammar.productions().begin(),
                      grammar.productions().end(), CNFRule{"S0", {"a"}}),
            grammar.productions().end());
  EXPECT_TRUE(std::none_of(
      grammar.productions().begin(), grammar.productions().end(),
      [&](const CNFRule &rule) {
        return rule.rhs.size() == 1 &&
               std::find(grammar.variables().begin(), grammar.variables().end(),
                         rule.rhs.front()) != grammar.variables().end();
      }));
}

TEST(ClassicalCNFTest, FreshVariablesScaleBeyondAlphabetSizedGrammars) {
  std::ostringstream text;
  text << "Terminals:\n  ";
  for (std::size_t i = 0; i < 40; ++i) {
    text << "t" << i << ' ';
  }
  text << "\nVariables:\n  S\nProductions:\n  S -> ";
  for (std::size_t i = 0; i < 40; ++i) {
    text << "t" << i << ' ';
  }
  text << ";\n";
  const auto path =
      createTempFile("lotus_classical_cnf_large_fresh.txt", text.str());

  const auto grammar = CNFGrammar::transformToSTBDU(path.string());
  EXPECT_GT(grammar.variables().size(), 26u);
  EXPECT_TRUE(std::all_of(grammar.productions().begin(),
                          grammar.productions().end(), [](const CNFRule &rule) {
                            return !rule.rhs.empty() && rule.rhs.size() <= 2;
                          }));
}

} // namespace lotus::cfl::classical
