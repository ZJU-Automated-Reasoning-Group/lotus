#include "CFL/CSIndex/ParallelTabulation.h"
#include "CFL/CSIndex/Tabulation.h"
#include "Utils/Parallel/ThreadPool.h"

#include <utility>
#include <vector>

#include <llvm/Support/CommandLine.h>
#include <gtest/gtest.h>

namespace {

Graph buildTestGraph() {
  Graph graph;
  for (int vertex = 0; vertex <= 6; ++vertex)
    graph.addVertex(vertex);

  graph.addEdge(0, 1);
  graph.addEdge(1, 2, 1);
  graph.addEdge(2, 3);
  graph.addEdge(3, 4);
  graph.addEdge(3, 5, -1);

  return graph;
}

std::vector<std::pair<int, int>> queryPairs() {
  return {
      {0, 4},
      {0, 5},
      {1, 4},
      {1, 5},
      {4, 5},
      {6, 0},
      {6, 6},
  };
}

TEST(ParallelTabulationHarnessTest, TcMatchesSequentialReference) {
  Graph graph = buildTestGraph();
  Tabulation serial(graph);
  ParallelTabulation parallel(graph, 4);

  EXPECT_DOUBLE_EQ(parallel.tc(), serial.tc());
}

TEST(ParallelTabulationHarnessTest, ReachabilityMatchesSequentialReference) {
  Graph graph = buildTestGraph();

  for (const auto &query : queryPairs()) {
    Tabulation serial(graph);
    ParallelTabulation parallel(graph, 4);
    EXPECT_EQ(parallel.reach(query.first, query.second),
              serial.reach(query.first, query.second))
        << "mismatch for query " << query.first << " -> " << query.second;
  }
}

TEST(ParallelTabulationHarnessTest,
     SerialModeWorksWhenLocalThreadCountExceedsAvailableWorkers) {
  ThreadPool *pool = ThreadPool::get();
  if (pool->hasWorkers())
    GTEST_SKIP() << "Serial fallback coverage is only meaningful without workers.";

  Graph graph = buildTestGraph();
  Tabulation serial(graph);
  ParallelTabulation requested_parallel(graph, 8);

  EXPECT_DOUBLE_EQ(requested_parallel.tc(), serial.tc());
}

TEST(ParallelTabulationHarnessTest,
     ParallelModeHandlesThreadCountsRelativeToVertexCount) {
  ThreadPool *pool = ThreadPool::get();
  if (!pool->hasWorkers())
    GTEST_SKIP() << "Parallel thread-count coverage requires worker threads.";

  Graph graph = buildTestGraph();
  const double expected_tc = Tabulation(graph).tc();

  ParallelTabulation fewer_threads(graph, 2);
  ParallelTabulation equal_threads(graph,
                                   static_cast<std::size_t>(graph.num_vertices()));
  ParallelTabulation more_threads(graph,
                                  static_cast<std::size_t>(graph.num_vertices() + 3));

  EXPECT_DOUBLE_EQ(fewer_threads.tc(), expected_tc);
  EXPECT_DOUBLE_EQ(equal_threads.tc(), expected_tc);
  EXPECT_DOUBLE_EQ(more_threads.tc(), expected_tc);
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv,
                                    "ParallelTabulation harness\n");
  return RUN_ALL_TESTS();
}
