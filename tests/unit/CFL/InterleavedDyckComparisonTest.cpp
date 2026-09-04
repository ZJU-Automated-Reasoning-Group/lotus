#include "CFL/InterleavedDyck/StagedBounds/Solver.h"
#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/MCFL/InterleavedDyck.h"
#include "CFL/InterleavedDyck/Unary/Solver.h"

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace lotus::cfl {
namespace {

TEST(InterleavedDyckComparisonTest, LoadsOneBenchmarkForAllApplicableSolvers) {
  const interleaved_dyck::Graph graph = interleaved_dyck::Graph::parseDotFile(
      std::string(INTERLEAVED_DYCK_DATASET_DIR) + "/faketaobao.dot");

  const auto approximation = interleaved_dyck::staged_bounds::Solver{}.analyze(
      graph, interleaved_dyck::staged_bounds::BenchmarkKind::Taint);
  const auto mcfl_result = interleaved_dyck::mcfl::InterleavedDyckSolver{}.solve(graph);

  ASSERT_EQ(mcfl_result.dimensions.size(), 2U);
  EXPECT_EQ(mcfl_result.dimensions[0].reachable_pairs.size(), 57U);
  EXPECT_EQ(mcfl_result.dimensions[1].reachable_pairs.size(), 59U);
  EXPECT_FALSE(approximation.underapproximation.empty());
  EXPECT_FALSE(approximation.on_demand.empty());
  for (const interleaved_dyck::mcfl::Pair &pair : mcfl_result.reachablePairs()) {
    EXPECT_NE(approximation.on_demand.count({pair.source, pair.target}), 0U)
        << "MCFL lower-bound pair escaped the final upper bound: "
        << pair.source << " -> " << pair.target;
  }

  // The published approximation corpus is directed. Both exact unary
  // algorithms must reject it rather than silently changing the problem.
  EXPECT_THROW(interleaved_dyck::unary::AdaptiveSolver{}.solve(graph),
               std::invalid_argument);
  EXPECT_THROW(interleaved_dyck::unary::FixedCounterSolver{}.solve(graph),
               std::invalid_argument);
}

} // namespace
} // namespace lotus::cfl
