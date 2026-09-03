#include "CFL/MutualRefinement/CnfGrammar.h"
#include "CFL/MutualRefinement/CnfGraph.h"
#include "CFL/MutualRefinement/Hasher.h"

#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <gtest/gtest.h>

namespace lotus::cfl::mutual_refinement {
namespace {

using UnaryRecord =
    std::unordered_map<Edge, std::unordered_set<int>, EdgeHasher>;
using BinaryRecord = std::unordered_map<
    Edge, std::unordered_set<std::tuple<int, int, int>, IntTripleHasher>,
    EdgeHasher>;

TEST(MutualRefinementFactorizedTracingTest,
     ReconstructsEveryBinaryPivotAndUnaryDerivation) {
  constexpr int A = 0;
  constexpr int B = 1;
  constexpr int Dead = 2;
  constexpr int S = 3;
  constexpr int Left = 4;
  constexpr int Right = 5;
  constexpr int Unary = 6;

  CnfGrammar grammar;
  grammar.addTerminal(A);
  grammar.addTerminal(B);
  grammar.addTerminal(Dead);
  grammar.addNonterminal(S);
  grammar.addNonterminal(Left);
  grammar.addNonterminal(Right);
  grammar.addNonterminal(Unary);
  grammar.addStartSymbol(S);
  grammar.addBinaryProduction(S, Left, Right);
  grammar.addUnaryProduction(S, Unary);
  grammar.addUnaryProduction(Left, A);
  grammar.addUnaryProduction(Right, B);
  grammar.addUnaryProduction(Unary, A);
  grammar.initFastIndices();

  const std::unordered_set<Edge, EdgeHasher> edges{
      std::make_tuple(0, A, 1), std::make_tuple(0, A, 2),
      std::make_tuple(1, B, 3), std::make_tuple(2, B, 3),
      std::make_tuple(0, A, 4), std::make_tuple(3, Dead, 4),
  };
  const std::unordered_set<Edge, EdgeHasher> roots{std::make_tuple(0, S, 3),
                                                   std::make_tuple(0, S, 4)};

  CnfGraph eager_graph;
  eager_graph.reinit(5, edges);
  UnaryRecord unary_record;
  BinaryRecord binary_record;
  const auto eager_result =
      eager_graph.runCFLReachability(grammar, unary_record, binary_record);
  ASSERT_NE(eager_result.count(std::make_tuple(0, S, 3)), 0U);
  ASSERT_NE(eager_result.count(std::make_tuple(0, S, 4)), 0U);
  const auto eager_closure =
      eager_graph.getEdgeClosure(grammar, roots, unary_record, binary_record);

  CnfGraph factorized_graph;
  factorized_graph.reinit(5, edges);
  EXPECT_EQ(factorized_graph.runCFLReachability(grammar), eager_result);
  const auto factorized_closure =
      factorized_graph.getFactorizedEdgeClosure(grammar, roots);

  const std::unordered_set<Edge, EdgeHasher> expected{
      std::make_tuple(0, A, 1), std::make_tuple(0, A, 2),
      std::make_tuple(1, B, 3), std::make_tuple(2, B, 3),
      std::make_tuple(0, A, 4),
  };
  EXPECT_EQ(eager_closure, expected);
  EXPECT_EQ(factorized_closure, eager_closure);
}

TEST(MutualRefinementFactorizedTracingTest,
     HandlesRecursiveAndEmptyProductions) {
  constexpr int A = 0;
  constexpr int Dead = 1;
  constexpr int S = 2;

  CnfGrammar grammar;
  grammar.addTerminal(A);
  grammar.addTerminal(Dead);
  grammar.addNonterminal(S);
  grammar.addStartSymbol(S);
  grammar.addEmptyProduction(S);
  grammar.addUnaryProduction(S, A);
  grammar.addBinaryProduction(S, S, S);
  grammar.initFastIndices();

  const std::unordered_set<Edge, EdgeHasher> edges{std::make_tuple(0, A, 1),
                                                   std::make_tuple(1, A, 2),
                                                   std::make_tuple(0, Dead, 2)};
  const std::unordered_set<Edge, EdgeHasher> roots{std::make_tuple(0, S, 2)};

  CnfGraph eager_graph;
  eager_graph.reinit(3, edges);
  UnaryRecord unary_record;
  BinaryRecord binary_record;
  eager_graph.runCFLReachability(grammar, unary_record, binary_record);
  const auto eager_closure =
      eager_graph.getEdgeClosure(grammar, roots, unary_record, binary_record);

  CnfGraph factorized_graph;
  factorized_graph.reinit(3, edges);
  factorized_graph.runCFLReachability(grammar);
  const auto factorized_closure =
      factorized_graph.getFactorizedEdgeClosure(grammar, roots);

  const std::unordered_set<Edge, EdgeHasher> expected{std::make_tuple(0, A, 1),
                                                      std::make_tuple(1, A, 2)};
  EXPECT_EQ(eager_closure, expected);
  EXPECT_EQ(factorized_closure, eager_closure);
}

} // namespace
} // namespace lotus::cfl::mutual_refinement
