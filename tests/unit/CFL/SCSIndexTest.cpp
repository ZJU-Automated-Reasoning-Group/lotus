#include "CFL/CSIndex/SCS/FactorizedIndex.h"
#include "CFL/CSIndex/FLARE/GraphAlgorithms.h"
#include "CFL/CSIndex/FLARE/Tabulation/Sequential.h"

#include <set>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

namespace flare = lotus::cfl::cs_index::flare;
namespace scs = lotus::cfl::cs_index::scs;

using GraphAlgorithms = flare::GraphAlgorithms;
using SequentialTabulation = flare::tabulation::Sequential;
using BatchQuery = scs::BatchQuery;
using Edge = scs::Edge;
using FactorizedIndex = scs::FactorizedIndex;
using Graph = scs::Graph;
using Index = scs::Index;
using IndexOptions = scs::IndexOptions;
using IndexStats = scs::IndexStats;
using PolicyAutomaton = scs::PolicyAutomaton;
using ProductConstruction = scs::ProductConstruction;

namespace {

constexpr int SOURCE_EVENT = 1;
constexpr int SANITIZE_EVENT = 2;
constexpr int SINK_EVENT = 3;

PolicyAutomaton buildTaintPolicy() {
  PolicyAutomaton policy(3, 0);
  policy.addAcceptingState(2);

  policy.addTransition(0, SOURCE_EVENT, 1);
  policy.addTransition(1, SOURCE_EVENT, 1);
  policy.addTransition(2, SOURCE_EVENT, 2);

  policy.addTransition(0, SANITIZE_EVENT, 0);
  policy.addTransition(1, SANITIZE_EVENT, 0);
  policy.addTransition(2, SANITIZE_EVENT, 2);

  policy.addTransition(0, SINK_EVENT, 0);
  policy.addTransition(1, SINK_EVENT, 2);
  policy.addTransition(2, SINK_EVENT, 2);
  return policy;
}

void addVertex(Graph &graph, int id, int function) {
  graph.addVertex(id, function);
}

void addNestedPath(Graph &graph, bool sanitized, bool matched_returns = true,
                   int offset = 0) {
  addVertex(graph, offset + 0, 0 + offset);
  addVertex(graph, offset + 1, 0 + offset);
  addVertex(graph, offset + 2, 1 + offset);
  addVertex(graph, offset + 3, 2 + offset);
  addVertex(graph, offset + 4, 1 + offset);
  addVertex(graph, offset + 5, 0 + offset);
  addVertex(graph, offset + 6, 0 + offset);

  graph.addEdge(offset + 0, offset + 1, 0, SOURCE_EVENT);
  graph.addEdge(offset + 1, offset + 2, 1, 0);
  graph.addEdge(offset + 2, offset + 3, 2, sanitized ? SANITIZE_EVENT : 0);
  graph.addEdge(offset + 3, offset + 4, matched_returns ? -2 : -3, 0);
  graph.addEdge(offset + 4, offset + 5, -1, 0);
  graph.addEdge(offset + 5, offset + 6, 0, SINK_EVENT);
}

Graph buildNestedPath(bool sanitized, bool matched_returns = true,
                         int offset = 0) {
  Graph graph;
  addNestedPath(graph, sanitized, matched_returns, offset);
  return graph;
}

flare::Graph structuralGraph(const Graph &source) {
  flare::Graph graph;
  for (int vertex = 0; vertex < source.num_vertices(); ++vertex) {
    graph.addVertex(vertex);
    graph.at(vertex).func_id = source.vertex(vertex).func_id;
  }
  for (const Edge &edge : source.edges()) {
    if (edge.structural_label == 0)
      graph.addEdge(edge.source, edge.target);
    else
      graph.addEdge(edge.source, edge.target, edge.structural_label);
  }
  return graph;
}

bool flareReachable(const Graph &source, int start, int target) {
  flare::Graph graph = structuralGraph(source);
  const int original_size = graph.num_vertices();
  graph.build_summary_edges();
  graph.to_indexing_graph();
  return GraphAlgorithms::DFSReach(graph, start, target + original_size);
}

TEST(SCSIndexTest, DistinguishesSanitizedAndUnsanitizedBalancedPaths) {
  PolicyAutomaton policy = buildTaintPolicy();
  Graph sanitized = buildNestedPath(true);
  Graph unsanitized = buildNestedPath(false);

  Index sanitized_index(sanitized, policy, {0}, {6});
  Index unsanitized_index(unsanitized, policy, {0}, {6});

  EXPECT_FALSE(sanitized_index.reachable(0, 6));
  EXPECT_TRUE(unsanitized_index.reachable(0, 6));
  EXPECT_TRUE(flareReachable(sanitized, 0, 6));
}

TEST(SCSIndexTest, RejectsMismatchedReturnsWithAnAcceptingPolicyRun) {
  Graph graph = buildNestedPath(false, false);
  Index index(graph, buildTaintPolicy(), {0}, {6});

  EXPECT_FALSE(index.reachable(0, 6));
  const int accepting_sink = index.productVertex(6, 2);
  ASSERT_GE(accepting_sink, 0);
  EXPECT_TRUE(GraphAlgorithms::DFSReach(index.productGraph(),
                                  index.productVertex(0, 0), accepting_sink));
}

TEST(SCSIndexTest, RequiresBothConstraintsOnTheSamePath) {
  Graph graph;
  for (int vertex = 0; vertex <= 9; ++vertex)
    graph.addVertex(vertex, vertex < 2 || vertex == 9 ? 0 : vertex);

  graph.addEdge(0, 1, 0, SOURCE_EVENT);

  // Context-valid but sanitized.
  graph.addEdge(1, 2, 1, 0);
  graph.addEdge(2, 3, 0, SANITIZE_EVENT);
  graph.addEdge(3, 4, -1, 0);
  graph.addEdge(4, 9, 0, SINK_EVENT);

  // Policy-accepting but context-invalid.
  graph.addEdge(1, 5, 2, 0);
  graph.addEdge(5, 6, 0, 0);
  graph.addEdge(6, 7, -3, 0);
  graph.addEdge(7, 9, 0, SINK_EVENT);

  Index index(graph, buildTaintPolicy(), {0}, {9});
  EXPECT_FALSE(index.reachable(0, 9));
  EXPECT_TRUE(flareReachable(graph, 0, 9));
  EXPECT_TRUE(GraphAlgorithms::DFSReach(index.productGraph(),
                                  index.productVertex(0, 0),
                                  index.productVertex(9, 2)));
}

TEST(SCSIndexTest, LazyAndExplicitProductsAreEquivalent) {
  Graph graph = buildNestedPath(false);
  graph.addVertex(7, 7);
  graph.addVertex(8, 7);
  graph.addEdge(7, 8);

  IndexOptions lazy_options;
  lazy_options.product_construction = ProductConstruction::Lazy;
  IndexOptions explicit_options;
  explicit_options.product_construction = ProductConstruction::Explicit;

  Index lazy(graph, buildTaintPolicy(), {0}, {6}, lazy_options);
  Index explicit_index(graph, buildTaintPolicy(), {0}, {6},
                          explicit_options);

  EXPECT_EQ(lazy.reachable(0, 6), explicit_index.reachable(0, 6));
  EXPECT_LT(lazy.stats().materialized_product_states,
            explicit_index.stats().materialized_product_states);
  EXPECT_EQ(explicit_index.stats().materialized_product_states,
            explicit_index.stats().explicit_product_states);
}

TEST(SCSIndexTest, SupportsDirectNfaProducts) {
  constexpr int START = 10;
  constexpr int FINISH = 11;
  Graph graph;
  graph.addVertex(0, 0);
  graph.addVertex(1, 0);
  graph.addVertex(2, 0);
  graph.addEdge(0, 1, 0, START);
  graph.addEdge(1, 2, 0, FINISH);

  PolicyAutomaton policy(2, 0, PolicyAutomaton::Kind::NFA);
  policy.addAcceptingState(1);
  policy.addTransition(0, START, 0);
  policy.addTransition(0, START, 1);
  policy.addTransition(1, FINISH, 1);

  Index index(graph, policy, {0}, {2});
  EXPECT_TRUE(index.reachable(0, 2));
}

TEST(SCSIndexTest, PreservesCollidingParallelStructuralRoles) {
  Graph graph;
  graph.addVertex(0, 0);
  graph.addVertex(1, 1);
  graph.addVertex(2, 0);
  graph.addVertex(3, 0);

  // Insert the mismatching call first so the valid call must be normalized.
  graph.addEdge(0, 1, 2, SOURCE_EVENT);
  graph.addEdge(0, 1, 1, SOURCE_EVENT);
  graph.addEdge(1, 2, -1, 0);
  graph.addEdge(2, 3, 0, SINK_EVENT);

  IndexOptions options;
  options.retain_witnesses = true;
  Index index(graph, buildTaintPolicy(), {0}, {3}, options);
  EXPECT_GT(index.stats().normalized_product_edges, 0U);
  EXPECT_TRUE(GraphAlgorithms::DFSReach(index.indexingGraph(), index.startVertex(0),
                                  index.acceptVertex(3)));
  EXPECT_TRUE(index.reachable(0, 3));
  EXPECT_TRUE(index.witness(0, 3).has_value());
}

TEST(SCSIndexTest, BatchEndpointsMatchTheDisjunctionOfPointQueries) {
  Graph graph;
  addNestedPath(graph, false, true, 0);
  addNestedPath(graph, true, true, 10);

  BatchQuery batch{"all", {0, 10}, {6, 16}};
  Index index(graph, buildTaintPolicy(), {0, 10}, {6, 16}, {}, {batch});

  bool disjunction = false;
  for (int source : batch.sources) {
    for (int sink : batch.sinks)
      disjunction = disjunction || index.reachable(source, sink);
  }
  EXPECT_EQ(index.reachableBatch("all"), disjunction);
  EXPECT_TRUE(disjunction);
}

PolicyAutomaton buildCategoryPolicy(int source_event, int sink_event,
                                    const std::set<int> &alphabet) {
  PolicyAutomaton policy(3, 0);
  policy.addAcceptingState(2);
  for (int event : alphabet) {
    if (event != source_event && event != sink_event) {
      policy.addIdentityTransitions(event);
      continue;
    }
    if (event == source_event) {
      policy.addTransition(0, event, 1);
      policy.addTransition(1, event, 1);
      policy.addTransition(2, event, 2);
    } else {
      policy.addTransition(0, event, 0);
      policy.addTransition(1, event, 2);
      policy.addTransition(2, event, 2);
    }
  }
  return policy;
}

TEST(SCSIndexTest, FactorizedCategoriesMatchTheJointDisjunctivePolicy) {
  constexpr int SQL_SOURCE = 20;
  constexpr int SQL_SINK = 21;
  constexpr int HTML_SOURCE = 22;
  constexpr int HTML_SINK = 23;
  const std::set<int> alphabet = {SQL_SOURCE, SQL_SINK, HTML_SOURCE, HTML_SINK};

  Graph graph;
  graph.addVertex(0, 0);
  graph.addVertex(1, 0);
  graph.addVertex(2, 0);
  graph.addEdge(0, 1, 0, SQL_SOURCE);
  graph.addEdge(1, 2, 0, SQL_SINK);

  PolicyAutomaton sql = buildCategoryPolicy(SQL_SOURCE, SQL_SINK, alphabet);
  PolicyAutomaton html = buildCategoryPolicy(HTML_SOURCE, HTML_SINK, alphabet);
  Index sql_index(graph, sql, {0}, {2});
  Index html_index(graph, html, {0}, {2});
  FactorizedIndex factorized({&sql_index, &html_index});

  PolicyAutomaton joint(9, 0);
  for (int sql_state = 0; sql_state < 3; ++sql_state) {
    for (int html_state = 0; html_state < 3; ++html_state) {
      const int joint_state = sql_state * 3 + html_state;
      if (sql_state == 2 || html_state == 2)
        joint.addAcceptingState(joint_state);
      for (int event : alphabet) {
        const int next_sql = sql.successors(sql_state, event).front();
        const int next_html = html.successors(html_state, event).front();
        joint.addTransition(joint_state, event, next_sql * 3 + next_html);
      }
    }
  }
  Index joint_index(graph, joint, {0}, {2});

  EXPECT_EQ(factorized.reachable(0, 2), joint_index.reachable(0, 2));
  EXPECT_TRUE(joint_index.reachable(0, 2));
}

TEST(SCSIndexTest, IndexedAnswerMatchesProductTabulation) {
  Graph graph = buildNestedPath(false);
  Index index(graph, buildTaintPolicy(), {0}, {6});

  flare::Graph product = index.productGraph();
  product.build_summary_edges();
  product.add_summary_edges();
  SequentialTabulation tabulation(product);

  const bool baseline =
      tabulation.reach(index.productVertex(0, 0), index.productVertex(6, 2));
  EXPECT_EQ(index.reachable(0, 6), baseline);
}

TEST(SCSIndexTest, ReconstructsAndReplaysAWitness) {
  Graph graph = buildNestedPath(false);
  IndexOptions options;
  options.retain_witnesses = true;
  Index index(graph, buildTaintPolicy(), {0}, {6}, options);

  const auto witness = index.witness(0, 6);
  ASSERT_TRUE(witness.has_value());
  EXPECT_EQ(witness->base_vertices, (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));
  EXPECT_TRUE(witness->context_valid);
  EXPECT_TRUE(witness->policy_accepting);
}

TEST(SCSIndexTest, RejectsIncompleteDfaPolicies) {
  Graph graph;
  graph.addVertex(0, 0);
  graph.addVertex(1, 0);
  graph.addEdge(0, 1, 0, SOURCE_EVENT);

  PolicyAutomaton incomplete(2, 0);
  incomplete.addAcceptingState(1);
  incomplete.addTransition(0, SOURCE_EVENT, 1);
  EXPECT_THROW(Index(graph, incomplete, {0}, {1}), std::invalid_argument);
}

TEST(SCSIndexTest, NormalizesSourceAndSinkVertexEventsToEdges) {
  Graph graph;
  graph.addVertex(0, 0);
  const int source = graph.addEventPredecessor(0, SOURCE_EVENT);
  const int sink = graph.addEventSuccessor(0, SINK_EVENT);

  Index index(graph, buildTaintPolicy(), {source}, {sink});
  EXPECT_TRUE(index.reachable(source, sink));
}

TEST(SCSIndexTest, RecordsConstructionAndQueryMetrics) {
  Graph graph;
  addNestedPath(graph, false, true, 0);
  addNestedPath(graph, true, true, 10);

  IndexOptions options;
  options.retain_witnesses = true;
  BatchQuery batch{"all", {0, 10}, {6, 16}};
  Index index(graph, buildTaintPolicy(), {0, 10}, {6, 16}, options, {batch});

  const IndexStats &construction = index.stats();
  EXPECT_EQ(construction.base_vertices, 17U);
  EXPECT_EQ(construction.base_edges, 12U);
  EXPECT_EQ(construction.policy_states, 3U);
  EXPECT_EQ(construction.accepting_policy_states, 1U);
  EXPECT_EQ(construction.indexed_sources, 2U);
  EXPECT_EQ(construction.indexed_sinks, 2U);
  EXPECT_EQ(construction.indexed_batches, 1U);
  EXPECT_GT(construction.total_construction_time_ns, 0U);
  EXPECT_GT(construction.product_vertices, 0U);
  EXPECT_GT(construction.flare_vertices, construction.product_vertices);
  EXPECT_GT(construction.indexing_vertices, construction.flare_vertices);
  EXPECT_GT(construction.dag_vertices, 0U);
  EXPECT_GT(construction.materializedProductFraction(), 0.0);
  EXPECT_LE(construction.materializedProductFraction(), 1.0);

  EXPECT_TRUE(index.reachable(0, 6));
  EXPECT_FALSE(index.reachable(10, 16));
  EXPECT_TRUE(index.reachableBatch("all"));
  EXPECT_TRUE(index.witness(0, 6).has_value());
  EXPECT_FALSE(index.witness(10, 16).has_value());

  const IndexStats &queries = index.stats();
  EXPECT_EQ(queries.point_queries.queries, 2U);
  EXPECT_EQ(queries.point_queries.positive_queries, 1U);
  EXPECT_EQ(queries.batch_queries.queries, 1U);
  EXPECT_EQ(queries.batch_queries.positive_queries, 1U);
  EXPECT_EQ(queries.witness_queries.queries, 2U);
  EXPECT_EQ(queries.witness_queries.positive_queries, 1U);
  EXPECT_LE(queries.point_queries.max_time_ns,
            queries.point_queries.total_time_ns);
  EXPECT_LE(queries.batch_queries.max_time_ns,
            queries.batch_queries.total_time_ns);
  EXPECT_LE(queries.witness_queries.max_time_ns,
            queries.witness_queries.total_time_ns);
  EXPECT_GE(queries.point_queries.averageMicroseconds(), 0.0);

  std::ostringstream header;
  std::ostringstream row;
  IndexStats::writeCsvHeader(header);
  queries.writeCsvRow(row);
  const std::string header_text = header.str();
  const std::string row_text = row.str();
  EXPECT_NE(header_text.find("total_construction_time_ns"), std::string::npos);
  EXPECT_NE(header_text.find("point_average_us"), std::string::npos);
  EXPECT_EQ(std::count(header_text.begin(), header_text.end(), ','),
            std::count(row_text.begin(), row_text.end(), ','));
}

} // namespace
