#include "CFL/InterleavedDyckApproximation/InterleavedDyckApproximation.h"
#include "CFL/InterleavedDyckCore/Graph.h"
#include "CFL/MCFL/InterleavedDyck.h"
#include "CFL/UnaryInterleavedDyck/UnaryInterleavedDyck.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace lotus::cfl {
namespace {

TEST(InterleavedDyckComparisonTest, LoadsOneBenchmarkForAllApplicableSolvers) {
  const interleaved_dyck::Graph graph = interleaved_dyck::Graph::parseDotFile(
      std::string(INTERLEAVED_DYCK_BENCHMARK_DIR) + "/faketaobao.dot");

  const auto approximation = interleaved_dyck_approximation::Solver{}.analyze(
      graph, interleaved_dyck_approximation::BenchmarkKind::Taint);
  const auto mcfl_result = mcfl::InterleavedDyckSolver{}.solve(graph);

  ASSERT_EQ(mcfl_result.dimensions.size(), 2U);
  EXPECT_EQ(mcfl_result.dimensions[0].reachable_pairs.size(), 57U);
  EXPECT_EQ(mcfl_result.dimensions[1].reachable_pairs.size(), 59U);
  EXPECT_FALSE(approximation.underapproximation.empty());
  EXPECT_FALSE(approximation.on_demand.empty());
  for (const mcfl::Pair &pair : mcfl_result.reachablePairs()) {
    EXPECT_NE(approximation.on_demand.count({pair.source, pair.target}), 0U)
        << "MCFL lower-bound pair escaped the final upper bound: "
        << pair.source << " -> " << pair.target;
  }

  // The published approximation corpus is directed. Both exact unary
  // algorithms must reject it rather than silently changing the problem.
  EXPECT_THROW(unary_interleaved_dyck::AdaptiveSolver{}.solve(graph),
               std::invalid_argument);
  EXPECT_THROW(unary_interleaved_dyck::FixedCounterSolver{}.solve(graph),
               std::invalid_argument);
}

} // namespace
} // namespace lotus::cfl
