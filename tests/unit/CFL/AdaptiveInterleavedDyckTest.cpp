#include "CFL/AdaptiveInterleavedDyck/AdaptiveInterleavedDyck.h"

#include "CFL/InterleavedDyckCore/Graph.h"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::adaptive_interleaved_dyck {
namespace {

using interleaved_dyck::Edge;
using interleaved_dyck::Graph;
using interleaved_dyck::Label;
using interleaved_dyck::LabelKind;
using interleaved_dyck::Vertex;

Graph bidirectedLinearGraph(const std::vector<std::string> &labels) {
  Graph graph;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    const Vertex source = static_cast<Vertex>(i);
    const Vertex target = static_cast<Vertex>(i + 1);
    const Label label = Label::parse(labels[i]);
    graph.addEdge(source, target, label);
    graph.addEdge(target, source, label.complement());
  }
  return graph;
}

bool advance(const Label &label, std::size_t counter_bound, std::size_t &first,
             std::size_t &second) {
  switch (label.kind) {
  case LabelKind::OpenParenthesis:
    if (first == counter_bound) {
      return false;
    }
    ++first;
    return true;
  case LabelKind::CloseParenthesis:
    if (first == 0) {
      return false;
    }
    --first;
    return true;
  case LabelKind::OpenBracket:
    if (second == counter_bound) {
      return false;
    }
    ++second;
    return true;
  case LabelKind::CloseBracket:
    if (second == 0) {
      return false;
    }
    --second;
    return true;
  case LabelKind::Neutral:
    return true;
  }
  return false;
}

bool bruteShallowConnected(const Graph &graph, Vertex source, Vertex target,
                           std::size_t threshold, std::size_t counter_bound) {
  const std::size_t width = counter_bound + 1;
  const auto state = [width](Vertex vertex, std::size_t first,
                             std::size_t second) {
    return (static_cast<std::size_t>(vertex) * width + first) * width + second;
  };
  std::vector<unsigned char> seen(graph.vertices().size() * width * width);
  std::deque<std::array<std::size_t, 3>> worklist;
  seen[state(source, 0, 0)] = true;
  worklist.push_back({static_cast<std::size_t>(source), 0, 0});

  while (!worklist.empty()) {
    const auto current = worklist.front();
    worklist.pop_front();
    if (current[0] == static_cast<std::size_t>(target) && current[1] == 0 &&
        current[2] == 0) {
      return true;
    }
    for (const Edge &edge : graph.edges()) {
      if (edge.source != static_cast<Vertex>(current[0])) {
        continue;
      }
      std::size_t first = current[1];
      std::size_t second = current[2];
      if (!advance(edge.label, counter_bound, first, second) ||
          std::min(first, second) > threshold) {
        continue;
      }
      const std::size_t next = state(edge.target, first, second);
      if (!seen[next]) {
        seen[next] = true;
        worklist.push_back(
            {static_cast<std::size_t>(edge.target), first, second});
      }
    }
  }
  return false;
}

std::vector<std::vector<unsigned char>>
bruteBoundedRelation(const Graph &graph, std::size_t vertex_count,
                     std::size_t counter_bound) {
  const std::size_t width = counter_bound + 1;
  const std::size_t state_count = vertex_count * width * width;
  const auto state = [width](std::size_t vertex, std::size_t first,
                             std::size_t second) {
    return (vertex * width + first) * width + second;
  };

  std::vector<std::vector<Edge>> outgoing(vertex_count);
  for (const Edge &edge : graph.edges()) {
    outgoing.at(static_cast<std::size_t>(edge.source)).push_back(edge);
  }

  std::vector<std::vector<unsigned char>> relation(
      vertex_count, std::vector<unsigned char>(vertex_count, false));
  std::vector<unsigned char> seen(state_count);
  std::deque<std::array<std::size_t, 3>> worklist;
  for (std::size_t source = 0; source < vertex_count; ++source) {
    std::fill(seen.begin(), seen.end(), false);
    worklist.clear();
    seen[state(source, 0, 0)] = true;
    worklist.push_back({source, 0, 0});

    while (!worklist.empty()) {
      const auto current = worklist.front();
      worklist.pop_front();
      for (const Edge &edge : outgoing[current[0]]) {
        std::size_t first = current[1];
        std::size_t second = current[2];
        if (!advance(edge.label, counter_bound, first, second)) {
          continue;
        }
        const std::size_t next =
            state(static_cast<std::size_t>(edge.target), first, second);
        if (!seen[next]) {
          seen[next] = true;
          worklist.push_back(
              {static_cast<std::size_t>(edge.target), first, second});
        }
      }
    }

    for (std::size_t target = 0; target < vertex_count; ++target) {
      relation[source][target] = seen[state(target, 0, 0)];
    }
  }
  return relation;
}

TEST(AdaptiveInterleavedDyckTest, DistinguishesShallowThresholds) {
  const Graph graph = bidirectedLinearGraph({"+1", "+2", "-1", "-2"});
  const AdaptiveInterleavedDyckSolver solver;

  EXPECT_FALSE(solver.solveShallow(graph, 0).connected(0, 4));
  EXPECT_TRUE(solver.solveShallow(graph, 1).connected(0, 4));
}

TEST(AdaptiveInterleavedDyckTest, SwitchesBetweenBothShallowArms) {
  const Graph graph = bidirectedLinearGraph(
      {"+2", "+2", "-2", "+1", "+1", "-1", "+2", "-1", "-2", "-2"});
  const AdaptiveInterleavedResult result =
      AdaptiveInterleavedDyckSolver{}.solveShallow(graph, 1);

  EXPECT_TRUE(result.connected(0, 10));
  EXPECT_EQ(result.stats().vertical_control_states, 22U);
  EXPECT_EQ(result.stats().horizontal_control_states, 22U);
}

TEST(AdaptiveInterleavedDyckTest, HandlesRepeatedArmAlternation) {
  const Graph graph = bidirectedLinearGraph(
      {"+2", "+2", "-2", "+1", "+1", "-1", "+2", "-2", "+1", "-1", "-1", "-2"});
  const AdaptiveInterleavedDyckSolver solver;

  EXPECT_FALSE(solver.solveShallow(graph, 0).connected(0, 12));
  EXPECT_TRUE(solver.solveShallow(graph, 1).connected(0, 12));
}

TEST(AdaptiveInterleavedDyckTest,
     ClosingSelfEdgeTriggersCascadingComponentMerges) {
  Graph graph;
  graph.addEdge(0, 0, Label::closeBracket(0));
  graph.addEdge(0, 0, Label::openBracket(0));
  graph.addEdge(0, 1, Label::closeBracket(0));
  graph.addEdge(1, 0, Label::openBracket(0));
  graph.addEdge(1, 2, Label::closeBracket(0));
  graph.addEdge(2, 1, Label::openBracket(0));

  const AdaptiveInterleavedResult shallow =
      AdaptiveInterleavedDyckSolver{}.solveShallow(graph, 0);
  EXPECT_TRUE(shallow.connected(0, 1));
  EXPECT_TRUE(shallow.connected(1, 2));

  const AdaptiveInterleavedResult exact =
      AdaptiveInterleavedDyckSolver{}.solve(graph);
  EXPECT_TRUE(exact.connected(0, 2));
  EXPECT_EQ(exact.stats().quotient_vertices, 1U);
}

TEST(AdaptiveInterleavedDyckTest, ProjectsTypedDelimiterIdsToUnaryCounters) {
  Graph graph;
  const std::vector<std::string> labels = {"op--7", "ob--9", "cp--2", "cb--4"};
  const std::vector<std::string> reverse = {"cp--11", "cb--3", "op--5",
                                            "ob--8"};
  for (std::size_t i = 0; i < labels.size(); ++i) {
    graph.addEdge(static_cast<Vertex>(i), static_cast<Vertex>(i + 1),
                  Label::parse(labels[i]));
    graph.addEdge(static_cast<Vertex>(i + 1), static_cast<Vertex>(i),
                  Label::parse(reverse[i]));
  }

  AdaptiveInterleavedOptions direct;
  direct.sparsify = false;
  const AdaptiveInterleavedResult result =
      AdaptiveInterleavedDyckSolver{}.solve(graph, direct);
  EXPECT_TRUE(result.connected(0, 4));
  EXPECT_EQ(result.stats().threshold, 30U);
}

TEST(AdaptiveInterleavedDyckTest, RejectsNonBidirectedInput) {
  Graph directed;
  directed.addEdge(0, 1, Label::openParenthesis(0));
  EXPECT_THROW(AdaptiveInterleavedDyckSolver{}.solve(directed),
               std::invalid_argument);
  EXPECT_THROW(Label::parse("mystery"), std::invalid_argument);
}

TEST(AdaptiveInterleavedDyckTest,
     ExplicitBidirectingProducesMarkedSoundOverapproximation) {
  Graph directed;
  directed.addEdge(0, 1, Label::openParenthesis(0));
  directed.addEdge(1, 2, Label::closeParenthesis(0));

  AdaptiveInterleavedOptions options;
  options.input_policy = BidirectedInputPolicy::AddMissingReverseEdges;
  const AdaptiveInterleavedResult result =
      AdaptiveInterleavedDyckSolver{}.solve(directed, options);

  EXPECT_TRUE(result.connected(0, 2));
  EXPECT_FALSE(result.stats().input_was_bidirected);
  EXPECT_TRUE(result.stats().overapproximates_original);
  EXPECT_EQ(result.stats().added_reverse_arcs, 2U);
  EXPECT_EQ(result.stats().input_arcs, 2U);
}

TEST(AdaptiveInterleavedDyckTest,
     MatchesQuadraticConstructionAcrossExhaustiveEdgeMasks) {
  struct CandidateEdge {
    Vertex source;
    Vertex target;
    const char *label;
  };
  constexpr std::size_t vertex_count = 3;
  constexpr std::size_t counter_bound =
      18 * vertex_count * vertex_count + 6 * vertex_count;
  const std::array<CandidateEdge, 7> candidates = {
      CandidateEdge{0, 1, "+1"},  CandidateEdge{0, 1, "+2"},
      CandidateEdge{1, 2, "-1"},  CandidateEdge{1, 2, "-2"},
      CandidateEdge{0, 2, "eps"}, CandidateEdge{0, 0, "+1"},
      CandidateEdge{2, 2, "+2"}};

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

    const auto expected =
        bruteBoundedRelation(graph, vertex_count, counter_bound);
    AdaptiveInterleavedOptions direct_options;
    direct_options.sparsify = false;
    const AdaptiveInterleavedResult direct =
        AdaptiveInterleavedDyckSolver{}.solve(graph, direct_options);
    const AdaptiveInterleavedResult sparsified =
        AdaptiveInterleavedDyckSolver{}.solve(graph);

    for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
         ++source) {
      for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
           ++target) {
        const bool oracle = expected[source][target] != 0;
        EXPECT_EQ(direct.connected(source, target), oracle);
        EXPECT_EQ(sparsified.connected(source, target), oracle);
      }
    }
  }
}

TEST(AdaptiveInterleavedDyckTest, MatchesExplicitSmallConfigurationGraphs) {
  constexpr std::size_t vertex_count = 4;
  constexpr std::size_t counter_bound = 48;
  const std::array<std::string, 5> labels = {"+1", "-1", "+2", "-2", "eps"};
  std::mt19937 generator(0xD1D1U);
  std::uniform_int_distribution<unsigned> vertex(0, vertex_count - 1);
  std::uniform_int_distribution<unsigned> label(0, labels.size() - 1);

  for (unsigned sample = 0; sample < 40; ++sample) {
    SCOPED_TRACE(::testing::Message() << "sample=" << sample);
    Graph graph;
    for (Vertex value = 0; value < static_cast<Vertex>(vertex_count); ++value) {
      graph.addVertex(value);
    }
    for (unsigned edge = 0; edge < 8; ++edge) {
      const Vertex source = vertex(generator);
      const Vertex target = vertex(generator);
      const Label forward = Label::parse(labels[label(generator)]);
      graph.addEdge(source, target, forward);
      graph.addEdge(target, source, forward.complement());
    }

    for (std::size_t threshold = 0; threshold <= 1; ++threshold) {
      const AdaptiveInterleavedResult result =
          AdaptiveInterleavedDyckSolver{}.solveShallow(graph, threshold);
      for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
           ++source) {
        for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
             ++target) {
          EXPECT_EQ(result.connected(source, target),
                    bruteShallowConnected(graph, source, target, threshold,
                                          counter_bound));
        }
      }
    }

    AdaptiveInterleavedOptions direct_options;
    direct_options.sparsify = false;
    const AdaptiveInterleavedResult direct =
        AdaptiveInterleavedDyckSolver{}.solve(graph, direct_options);
    const AdaptiveInterleavedResult sparsified =
        AdaptiveInterleavedDyckSolver{}.solve(graph);
    for (Vertex source = 0; source < static_cast<Vertex>(vertex_count);
         ++source) {
      for (Vertex target = 0; target < static_cast<Vertex>(vertex_count);
           ++target) {
        EXPECT_EQ(direct.connected(source, target),
                  sparsified.connected(source, target));
      }
    }
  }
}

} // namespace
} // namespace lotus::cfl::adaptive_interleaved_dyck
