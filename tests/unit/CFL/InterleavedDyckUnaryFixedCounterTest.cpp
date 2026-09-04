#include "CFL/InterleavedDyck/Core/Graph.h"
#include "CFL/InterleavedDyck/Unary/Adaptive.h"
#include "CFL/InterleavedDyck/Unary/FixedCounter.h"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::interleaved_dyck::unary {
namespace {

using interleaved_dyck::Graph;
using interleaved_dyck::Label;
using interleaved_dyck::Vertex;

Graph bidirectedLinearGraph(const std::vector<std::string> &labels) {
  Graph graph;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const Label label = Label::parse(labels[i]);
    const Vertex source = static_cast<Vertex>(i);
    const Vertex target = static_cast<Vertex>(i + 1);
    graph.addEdge(source, target, label);
    graph.addEdge(target, source, label.complement());
  }
  return graph;
}

TEST(InterleavedDyckUnaryFixedCounterTest,
     ImplementsThePaperFixedCounterConstruction) {
  const Graph graph = bidirectedLinearGraph({"+1", "+2", "-1", "-2"});
  FixedCounterOptions options;
  options.sparsify = false;
  const FixedCounterResult result = FixedCounterSolver{}.solve(graph, options);

  EXPECT_TRUE(result.connected(0, 4));
  EXPECT_EQ(result.stats().counter_bound, 480U);
  EXPECT_EQ(result.stats().control_states, 2405U);
  EXPECT_EQ(result.stats().translated_arcs, 3844U);
  EXPECT_EQ(result.stats().dyck.states, result.stats().control_states);
}

TEST(InterleavedDyckUnaryFixedCounterTest,
     SharesUnaryProjectionAndSparsification) {
  Graph graph;
  graph.addEdge(0, 1, Label::closeParenthesis(0));
  graph.addEdge(1, 0, Label::openParenthesis(0));
  graph.addEdge(0, 2, Label::closeParenthesis(0));
  graph.addEdge(2, 0, Label::openParenthesis(0));

  const FixedCounterResult result = FixedCounterSolver{}.solve(graph);
  EXPECT_TRUE(result.connected(1, 2));
  EXPECT_EQ(result.stats().input_vertices, 3U);
  EXPECT_EQ(result.stats().quotient_vertices, 2U);
  EXPECT_TRUE(result.stats().sparsified);
}

TEST(InterleavedDyckUnaryFixedCounterTest,
     MatchesAdaptiveAcrossExhaustiveSmallEdgeMasks) {
  struct CandidateEdge {
    Vertex source;
    Vertex target;
    const char *label;
  };
  constexpr std::size_t vertex_count = 3;
  const std::array<CandidateEdge, 6> candidates = {
      CandidateEdge{0, 1, "+1"},  CandidateEdge{0, 1, "+2"},
      CandidateEdge{1, 2, "-1"},  CandidateEdge{1, 2, "-2"},
      CandidateEdge{0, 2, "eps"}, CandidateEdge{0, 0, "+1"}};

  for (unsigned mask = 0; mask < (1U << candidates.size()); ++mask) {
    SCOPED_TRACE(::testing::Message() << "edge mask=" << mask);
    Graph graph;
    for (Vertex vertex = 0; vertex < static_cast<Vertex>(vertex_count);
         ++vertex) {
      graph.addVertex(vertex);
    }
    for (std::size_t edge = 0; edge < candidates.size(); ++edge) {
      if ((mask & (1U << edge)) == 0U) {
        continue;
      }
      const CandidateEdge &candidate = candidates[edge];
      const Label label = Label::parse(candidate.label);
      graph.addEdge(candidate.source, candidate.target, label);
      graph.addEdge(candidate.target, candidate.source, label.complement());
    }

    FixedCounterOptions fixed_options;
    fixed_options.sparsify = false;
    AdaptiveOptions adaptive_options;
    adaptive_options.sparsify = false;
    const FixedCounterResult fixed_counter =
        FixedCounterSolver{}.solve(graph, fixed_options);
    const auto adaptive = AdaptiveSolver{}.solve(graph, adaptive_options);
    for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
         ++source) {
      for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
           ++target) {
        EXPECT_EQ(fixed_counter.connected(source, target),
                  adaptive.connected(source, target));
      }
    }
  }
}

TEST(InterleavedDyckUnaryFixedCounterTest,
     ExplicitBidirectingMarksTheResultAsAnOverapproximation) {
  Graph directed;
  directed.addEdge(0, 1, Label::openParenthesis(0));
  directed.addEdge(1, 2, Label::closeParenthesis(0));

  EXPECT_THROW(FixedCounterSolver{}.solve(directed), std::invalid_argument);

  FixedCounterOptions options;
  options.input_policy =
      interleaved_dyck::BidirectedInputPolicy::AddMissingReverseEdges;
  const FixedCounterResult result =
      FixedCounterSolver{}.solve(directed, options);
  EXPECT_TRUE(result.connected(0, 2));
  EXPECT_TRUE(result.stats().overapproximates_original);
  EXPECT_EQ(result.stats().added_reverse_arcs, 2U);
}

} // namespace
} // namespace lotus::cfl::interleaved_dyck::unary
