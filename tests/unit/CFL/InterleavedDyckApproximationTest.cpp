#include "CFL/InterleavedDyckApproximation/InterleavedDyckApproximation.h"

#include <sstream>

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck_approximation {
namespace {

bool contains(const PairSet &pairs, Vertex source, Vertex target) {
  return pairs.count({source, target}) != 0U;
}

TEST(InterleavedDyckApproximationGraphTest,
     ParsesArtifactDotLabelsAndDeduplicatesEdges) {
  std::istringstream input("digraph G {\n"
                           "  10 -> 20 [label=\"op--7\"];\n"
                           "  10 -> 20 [label=\"op--7\"];\n"
                           "  20 -> 30 [label=\"normal\"];\n"
                           "}\n");
  const Graph graph = Graph::parseDot(input);

  ASSERT_EQ(graph.edges().size(), 2U);
  EXPECT_EQ(graph.edges()[0].label, Label::openParenthesis(7));
  EXPECT_EQ(graph.edges()[1].label, Label::neutral());
}

TEST(InterleavedDyckApproximationSolverTest,
     SeparatesUnionDyckUnderapproximationFromFullInterleavedDyck) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  graph.addEdge(3, 4, Label::closeBracket(0));

  const Solver solver;
  EXPECT_TRUE(contains(solver.intersection(graph), 0, 4));
  EXPECT_FALSE(contains(solver.underapproximation(graph), 0, 4));
  EXPECT_TRUE(contains(solver.mutualRefinement(graph), 0, 4));
  EXPECT_TRUE(
      contains(solver.projectedReachability(graph, Alphabet::Parenthesis,
                                            GrammarStrength::Parity),
               0, 4));
}

TEST(InterleavedDyckApproximationSolverTest,
     MutualRefinementRejectsDifferentWitnesses) {
  Graph graph;
  // Parenthesis-valid witness carrying an unmatched bracket.
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeParenthesis(0));
  // Bracket-valid witness carrying an unmatched parenthesis.
  graph.addEdge(0, 4, Label::openBracket(0));
  graph.addEdge(4, 5, Label::openParenthesis(0));
  graph.addEdge(5, 3, Label::closeBracket(0));

  const Solver solver;
  EXPECT_TRUE(contains(solver.intersection(graph), 0, 3));
  EXPECT_FALSE(contains(solver.mutualRefinement(graph), 0, 3));
}

TEST(InterleavedDyckApproximationSolverTest,
     FullPipelineKeepsAConcreteBalancedPath) {
  Graph graph;
  graph.addEdge(0, 1, Label::openParenthesis(0));
  graph.addEdge(1, 2, Label::openBracket(0));
  graph.addEdge(2, 3, Label::closeBracket(0));
  graph.addEdge(3, 4, Label::closeParenthesis(0));

  const ApproximationResult result = Solver{}.analyze(graph);
  EXPECT_TRUE(contains(result.intersection, 0, 4));
  EXPECT_TRUE(contains(result.underapproximation, 0, 4));
  EXPECT_TRUE(contains(result.mutual_refinement, 0, 4));
  EXPECT_TRUE(contains(result.stronger_grammar, 0, 4));
  EXPECT_TRUE(contains(result.on_demand, 0, 4));
}

TEST(InterleavedDyckApproximationSolverTest,
     ValueFlowPipelineAppliesSourceSinkCondition) {
  Graph graph;
  graph.addEdge(0, 1, Label::openBracket(0));
  graph.addEdge(1, 2, Label::neutral());
  graph.addEdge(2, 3, Label::closeBracket(0));

  const ApproximationResult result =
      Solver{}.analyze(graph, BenchmarkKind::ValueFlow);
  EXPECT_TRUE(contains(result.regularization, 0, 3));
  EXPECT_TRUE(contains(result.intersection, 0, 3));
  EXPECT_TRUE(contains(result.underapproximation, 0, 3));
  EXPECT_TRUE(contains(result.mutual_refinement, 0, 3));
  EXPECT_TRUE(contains(result.stronger_grammar, 0, 3));
  EXPECT_TRUE(contains(result.on_demand, 0, 3));
}

TEST(InterleavedDyckApproximationSolverTest, ParityRefinementIsComponentLocal) {
  Graph left;
  left.addEdge(998, 2273, Label::closeParenthesis(27));
  left.addEdge(998, 1713, Label::closeParenthesis(45));
  left.addEdge(991, 1003, Label::closeBracket(3));
  left.addEdge(1001, 991, Label::openBracket(2));
  left.addEdge(2311, 992, Label::openParenthesis(27));
  left.addEdge(992, 991, Label::openParenthesis(45));
  left.addEdge(991, 992, Label::openBracket(3));
  left.addEdge(1003, 1001, Label::closeBracket(3));
  left.addEdge(1001, 1011, Label::closeBracket(2));
  left.addEdge(992, 991, Label::openBracket(3));
  left.addEdge(1011, 998, Label::closeBracket(2));
  left.addEdge(1001, 1011, Label::openParenthesis(45));
  left.addEdge(2273, 1423, Label::openBracket(0));
  left.addEdge(1423, 1423, Label::closeBracket(0));

  Graph right;
  right.addEdge(1, 0, Label::openBracket(1));
  right.addEdge(3, 2, Label::closeBracket(1));

  Graph combined = left;
  for (const Edge &edge : right.edges()) {
    combined.addEdge(edge.source, edge.target, edge.label);
  }

  const Solver solver;
  PairSet separate = solver.mutualRefinement(left, GrammarStrength::Parity, 2,
                                             BenchmarkKind::Taint);
  const PairSet right_result = solver.mutualRefinement(
      right, GrammarStrength::Parity, 2, BenchmarkKind::Taint);
  separate.insert(right_result.begin(), right_result.end());
  const PairSet together = solver.mutualRefinement(
      combined, GrammarStrength::Parity, 2, BenchmarkKind::Taint);

  EXPECT_EQ(together.size(), separate.size());
  for (const Pair &pair : separate) {
    EXPECT_NE(together.count(pair), 0U);
  }
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck_approximation
