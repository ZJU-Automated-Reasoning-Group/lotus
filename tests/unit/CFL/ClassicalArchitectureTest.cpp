#include "CFL/Classical/Core/RecursiveStateMachine.h"
#include "CFL/Classical/Core/Validation.h"
#include "CFL/Classical/Solvers/Engines/PEARL/PearlEngine.h"
#include "CFL/Classical/Solvers/Engines/POCR/ClientGrammars.h"
#include "CFL/Classical/Solvers/Engines/POCR/FullyOrderedClosure.h"
#include "CFL/Classical/Solvers/Engines/POCR/PairedTreeClosure.h"
#include "CFL/Classical/Solvers/Engines/POCR/SpecializedEngines.h"
#include "CFL/Classical/Solvers/Engines/SQID/SqidEngine.h"
#include "CFL/Classical/Solvers/Engines/STG/StagedSolver.h"
#include "CFL/Classical/Solvers/Engines/TransitiveClosure.h"
#include "CFL/Classical/Solvers/Preprocessing/GraphSimplification.h"
#include "CFL/Classical/Solvers/Preprocessing/RSMFoldability.h"
#include "CFL/Classical/Solvers/SolverSession.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

namespace lotus::cfl::classical {
namespace {

using NamedEdge = std::tuple<std::string, std::size_t, std::size_t>;
using namespace lotus::cfl::classical::engines;

std::set<NamedEdge> solveWith(SolverBackend backend, LabeledGraph graph,
                              const Grammar &grammar) {
  SolverSession session(graph, grammar, backend);
  session.solve();
  std::set<NamedEdge> result;
  for (const RelationEdge &edge : session.relation().edges()) {
    result.emplace(grammar.symbolName(edge.symbol), edge.source, edge.target);
  }
  return result;
}

std::set<NamedEdge> solveReference(const LabeledGraph &graph,
                                   const Grammar &grammar) {
  const std::size_t node_count = graph.vertexCount();
  std::vector<std::vector<std::vector<bool>>> relation(
      grammar.symbolCount(),
      std::vector<std::vector<bool>>(node_count,
                                     std::vector<bool>(node_count, false)));
  for (const LabeledEdge &edge : graph.edges()) {
    relation[grammar.symbolId(edge.label)][edge.source][edge.target] = true;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[head, rules] : grammar.productions()) {
      const SymbolId lhs = grammar.symbolId(head);
      for (const auto &rule : rules) {
        if (rule.size() == 1 && rule.front() == Grammar::kEpsilonSymbol) {
          for (std::size_t node = 0; node < node_count; ++node) {
            if (!relation[lhs][node][node]) {
              relation[lhs][node][node] = true;
              changed = true;
            }
          }
        } else if (rule.size() == 1) {
          const SymbolId rhs = grammar.symbolId(rule.front());
          for (std::size_t source = 0; source < node_count; ++source) {
            for (std::size_t target = 0; target < node_count; ++target) {
              if (relation[rhs][source][target] &&
                  !relation[lhs][source][target]) {
                relation[lhs][source][target] = true;
                changed = true;
              }
            }
          }
        } else if (rule.size() == 2) {
          const SymbolId first = grammar.symbolId(rule[0]);
          const SymbolId second = grammar.symbolId(rule[1]);
          for (std::size_t source = 0; source < node_count; ++source) {
            for (std::size_t middle = 0; middle < node_count; ++middle) {
              if (!relation[first][source][middle]) {
                continue;
              }
              for (std::size_t target = 0; target < node_count; ++target) {
                if (relation[second][middle][target] &&
                    !relation[lhs][source][target]) {
                  relation[lhs][source][target] = true;
                  changed = true;
                }
              }
            }
          }
        }
      }
    }
  }

  std::set<NamedEdge> result;
  for (SymbolId symbol = 0; symbol < grammar.symbolCount(); ++symbol) {
    for (std::size_t source = 0; source < node_count; ++source) {
      for (std::size_t target = 0; target < node_count; ++target) {
        if (relation[symbol][source][target]) {
          result.emplace(grammar.symbolName(symbol), source, target);
        }
      }
    }
  }
  return result;
}

std::filesystem::path writeTemp(const std::string &name,
                                const std::string &content) {
  const auto path = std::filesystem::temp_directory_path() / name;
  std::ofstream output(path);
  output << content;
  return path;
}

TEST(ClassicalArchitectureTest, PreservesStartAndInstantiatesAttributes) {
  GrammarParseOptions options;
  options.variable_attributes['i'] = {3, 7};
  const auto grammar =
      Grammar::parseFromText("Start:\n"
                             "  S\n"
                             "Terminal:\n"
                             "  call ret\n"
                             "Variables:\n"
                             "  S\n"
                             "Productions:\n"
                             "  S -> call_i S ret_i | <epsilon>;\n",
                             options);

  EXPECT_EQ(grammar.startSymbol(), "S");
  EXPECT_TRUE(grammar.isNonterminal("S"));
  EXPECT_TRUE(grammar.isTerminal("call_3"));
  EXPECT_TRUE(grammar.isTerminal("ret_7"));
  EXPECT_FALSE(grammar.hasSymbol("call_i"));
  EXPECT_TRUE(grammar.validate().empty());
}

TEST(ClassicalArchitectureTest,
     GeneratedNonterminalsAreNeverClassifiedAsTerminals) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a __lotus_generated_1\n"
                             "Variables:\n  S\nProductions:\n"
                             "  S -> ( a ? ) * a a a;\n");

  for (const std::string &nonterminal : grammar.nonterminals()) {
    EXPECT_FALSE(grammar.isTerminal(nonterminal)) << nonterminal;
  }
  EXPECT_TRUE(grammar.validate().empty());
}

TEST(ClassicalArchitectureTest, ValidationRejectsSymbolKindIntersection) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  S a\nVariables:\n  S\n"
                             "Productions:\n  S -> a;\n");
  const auto issues = grammar.validate();
  EXPECT_TRUE(
      std::any_of(issues.begin(), issues.end(), [](const GrammarIssue &issue) {
        return issue.severity == GrammarIssueSeverity::Error &&
               issue.message.find("both terminal and nonterminal") !=
                   std::string::npos;
      }));
}

TEST(ClassicalArchitectureTest, CorrelatesAttributesAcrossProductionHeads) {
  GrammarParseOptions options;
  options.variable_attributes['i'] = {2, 5};
  const auto grammar =
      Grammar::parseFromText("Productions:\n  Edge_i -> call_i;\n", options);
  EXPECT_TRUE(grammar.isNonterminal("Edge_2"));
  EXPECT_TRUE(grammar.isNonterminal("Edge_5"));
  EXPECT_NE(grammar.unaryByRhs().find("call_2"), grammar.unaryByRhs().end());
  EXPECT_NE(grammar.unaryByRhs().find("call_5"), grammar.unaryByRhs().end());
}

TEST(ClassicalArchitectureTest, IntersectsPerKindAttributeDomains) {
  GrammarParseOptions options;
  options.symbol_attributes["call"] = {1, 2};
  options.symbol_attributes["ret"] = {2, 3};
  const auto grammar =
      Grammar::parseFromText("Productions:\n  S -> call_i ret_i;\n", options);

  EXPECT_TRUE(grammar.hasSymbol("call_2"));
  EXPECT_TRUE(grammar.hasSymbol("ret_2"));
  EXPECT_TRUE(grammar.isTerminal("call_1"));
  EXPECT_TRUE(grammar.isTerminal("ret_3"));
  EXPECT_EQ(grammar.binaryByFirst().count("call_1"), 0u);
  EXPECT_EQ(grammar.binaryBySecond().count("ret_3"), 0u);
}

TEST(ClassicalArchitectureTest, RejectsUnboundAttributeVariables) {
  EXPECT_THROW(Grammar::parseFromText("Productions:\n  S -> call_i ret_i;\n"),
               std::invalid_argument);
}

TEST(ClassicalArchitectureTest, RejectsExplosiveAttributeExpansion) {
  GrammarParseOptions options;
  options.variable_attributes['i'] = {1, 2, 3};
  options.variable_attributes['j'] = {4, 5, 6};
  options.max_attribute_expansions = 8;
  EXPECT_THROW(
      Grammar::parseFromText(
          "Productions:\n  S -> call_i ret_i load_j store_j;\n", options),
      std::length_error);
}

TEST(ClassicalArchitectureTest, InfersAttributeDomainsFromGraphLabels) {
  LabeledGraph graph;
  graph.addEdge("a", "b", "call_4");
  graph.addEdge("b", "c", "ret_4");
  graph.addEdge("c", "d", "call_9");
  graph.addEdge("d", "e", "ret_9");
  const auto grammar = Grammar::parseFromText(
      "Productions:\n  S -> call_i ret_i;\n", inferGrammarAttributes(graph));

  EXPECT_TRUE(grammar.hasSymbol("call_4"));
  EXPECT_TRUE(grammar.hasSymbol("call_9"));
  EXPECT_TRUE(grammar.hasSymbol("ret_4"));
  EXPECT_TRUE(grammar.hasSymbol("ret_9"));
}

TEST(ClassicalArchitectureTest, ParsesPocrGrammarAndAttributedEdgeLists) {
  const auto graph_path = writeTemp(
      "lotus_pocr_graph.vfg", "0\t1\tcall_i\t7\n1\t2\tret_i\t7\n2\t3\ta\n");
  const LabeledGraph graph = LabeledGraph::parseFromFile(
      graph_path.string(),
      GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  ASSERT_EQ(graph.edgeCount(), 3u);
  EXPECT_TRUE(
      graph.hasEdge(graph.vertexId("0"), graph.vertexId("1"), "call_7"));

  const auto extensionless_path = writeTemp("lotus_pocr_edges", "0\t1\ta\n");
  EXPECT_EQ(
      LabeledGraph::parseFromFile(extensionless_path.string(), GraphMode::Plain)
          .edgeCount(),
      1u);

  const auto grammar =
      Grammar::parseFromText("Production:\n"
                             "A\tA\tA\n"
                             "A\tcall_i\tR_i\n"
                             "R_i\tA\tret_i\n"
                             "A\ta\n"
                             "A\n\n"
                             "Insert:\nA, R_i\n\nFollow:\n\nCount:\nA\n",
                             inferGrammarAttributes(graph));
  EXPECT_TRUE(grammar.usesUnidirectionalMetadata());
  EXPECT_EQ(grammar.startSymbol(), "A");
  EXPECT_TRUE(grammar.isInsertSymbol("R_7"));
  EXPECT_TRUE(grammar.isCountSymbol("A"));
  EXPECT_TRUE(grammar.isTerminal("call_7"));
  EXPECT_TRUE(grammar.validate().empty());

  const Grammar rewritten = Grammar::parseFromText("S\ta\nS\tS\tS\n");
  EXPECT_EQ(rewritten.startSymbol(), "S");
  EXPECT_TRUE(rewritten.transitiveSymbols().count(rewritten.symbolId("S")) !=
              0);
}

TEST(ClassicalArchitectureTest,
     BuildsExactStandardAndRewrittenPocrClientGrammars) {
  LabeledGraph graph;
  graph.addVertex("n0");

  const Grammar standard_alias =
      buildPocrClientGrammar(PocrClientGrammar::StandardAlias, graph);
  const Grammar rewritten_alias =
      buildPocrClientGrammar(PocrClientGrammar::RewrittenAlias, graph);
  EXPECT_EQ(standard_alias.startSymbol(), "V");
  EXPECT_TRUE(standard_alias.isCountSymbol("V"));
  EXPECT_TRUE(
      standard_alias.transitiveSymbols().count(standard_alias.symbolId("A")));
  EXPECT_FALSE(
      rewritten_alias.transitiveSymbols().count(rewritten_alias.symbolId("A")));

  const Grammar standard_value_flow =
      buildPocrClientGrammar(PocrClientGrammar::StandardValueFlow, graph);
  const Grammar rewritten_value_flow =
      buildPocrClientGrammar(PocrClientGrammar::RewrittenValueFlow, graph);
  EXPECT_EQ(standard_value_flow.startSymbol(), "A");
  EXPECT_TRUE(standard_value_flow.isCountSymbol("A"));
  EXPECT_TRUE(standard_value_flow.transitiveSymbols().count(
      standard_value_flow.symbolId("A")));
  EXPECT_FALSE(rewritten_value_flow.transitiveSymbols().count(
      rewritten_value_flow.symbolId("A")));
}

TEST(ClassicalArchitectureTest,
     PocrUnboundIndexedHeadsUseOnlyTheZeroAttribute) {
  const auto graph_path = writeTemp("lotus_pocr_default_attribute.graph",
                                    "0\t1\ta\n1\t2\tcall_i\t7\n2\t3\tret_i\n");
  LabeledGraph graph = LabeledGraph::parseFromFile(
      graph_path.string(),
      GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  EXPECT_TRUE(graph.hasEdge(2, 3, "ret_0"));

  const Grammar grammar = Grammar::parseFromText("Production:\n"
                                                 "S\tX_i\n"
                                                 "X_i\tY_i\n"
                                                 "Y_i\ta\n"
                                                 "Z_i\tcall_i\n"
                                                 "W_i\tret_i\n",
                                                 inferGrammarAttributes(graph));
  SolverSession session(graph, grammar, SolverBackend::SparseBitVector);
  session.solve();
  EXPECT_TRUE(session.contains(0, 1, "Y_0"));
  EXPECT_TRUE(session.contains(0, 1, "X_0"));
  EXPECT_TRUE(session.contains(0, 1, "S"));
  EXPECT_TRUE(session.contains(1, 2, "Z_7"));
  EXPECT_FALSE(session.contains(0, 1, "Y_7"));
  EXPECT_FALSE(session.contains(0, 1, "X_7"));
}

TEST(ClassicalArchitectureTest,
     UnidirectionalMetadataSuppressesOnlyFutureJoinCandidates) {
  const auto grammar =
      Grammar::parseFromText("Production:\n"
                             "S\tX\tb\n"
                             "X\ta\n\n"
                             "Insert:\nS\n\nFollow:\nX\n\nCount:\nS\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");

  SolverSession session(graph, grammar,
                        SolverOptions{SolverBackend::SparseBitVector, true});
  const ReachabilityStats stats = session.solve();
  EXPECT_TRUE(session.contains(0, 1, "X"));
  EXPECT_TRUE(session.contains(0, 2, "S"));
  EXPECT_LT(stats.candidate_relation_edges, stats.relation_edges);
  EXPECT_EQ(stats.count_symbol_edges, 1u);
}

TEST(ClassicalArchitectureTest, ExecutesIndexedRecursiveStateMachineCalls) {
  const RecursiveStateMachine rsm =
      RecursiveStateMachine::parseFromText("q0\tenter\tB,q1\n"
                                           "B,q1\texit\tq2\n"
                                           "init:\tq0\n"
                                           "acpt:\tq2\n");

  const RsmGlobalState entered =
      rsm.transition(rsm.initialState(), rsm.parseLabel("enter_7"));
  ASSERT_TRUE(entered.valid());
  ASSERT_EQ(entered.boxes.size(), 1u);
  EXPECT_EQ(entered.boxes.back().index, 7u);

  const RsmGlobalState accepted =
      rsm.transition(entered, rsm.parseLabel("exit_7"));
  EXPECT_TRUE(rsm.isAccepting(accepted));
  EXPECT_TRUE(accepted.boxes.empty());
  EXPECT_FALSE(rsm.transition(entered, rsm.parseLabel("exit_8")).valid());
}

TEST(ClassicalArchitectureTest, IdentifiesRsmFoldableNodePairPatterns) {
  const RecursiveStateMachine rsm =
      RecursiveStateMachine::parseFromText("q0\ta\tq1\ninit:\tq0\nacpt:\tq1\n");
  const FoldabilityChecker checker(rsm);
  EXPECT_TRUE(checker.isFoldable(NodePairPattern::parse("0;0;;;;;;", rsm)));
  EXPECT_FALSE(checker.isFoldable(NodePairPattern::parse("0;1;;;;;;", rsm)));
  EXPECT_THROW(NodePairPattern::parse("0;0;too;short", rsm),
               std::invalid_argument);
}

TEST(ClassicalArchitectureTest, PortsAliasSccAndGraphFoldingPasses) {
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n0", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "d0", "d");
  graph.addEdge("n2", "d1", "d");

  const GraphSimplificationResult result =
      simplifyGraph(graph, {GraphSimplificationFlavor::Alias, true, true});
  EXPECT_LT(result.graph.vertexCount(), graph.vertexCount());
  EXPECT_EQ(result.representative[graph.vertexId("n0")],
            result.representative[graph.vertexId("n1")]);
  EXPECT_EQ(result.representative[graph.vertexId("d0")],
            result.representative[graph.vertexId("d1")]);
  EXPECT_GT(result.statistics.scc_nodes_merged, 0u);
  EXPECT_GT(result.statistics.common_dereference_nodes_merged, 0u);
}

TEST(ClassicalArchitectureTest,
     GraphFoldingMapsPocrLabelsToLotusPhysicalOrientations) {
  LabeledGraph alias_graph;
  alias_graph.addEdge("pointer", "object0", "addrbar");
  alias_graph.addEdge("object0", "pointer", "addr");
  alias_graph.addEdge("pointer", "object1", "addrbar");
  alias_graph.addEdge("object1", "pointer", "addr");
  const GraphSimplificationResult alias_result = simplifyGraph(
      alias_graph, {GraphSimplificationFlavor::Alias, false, true});
  EXPECT_EQ(alias_result.representative[alias_graph.vertexId("object0")],
            alias_result.representative[alias_graph.vertexId("object1")]);
  EXPECT_GT(alias_result.statistics.common_dereference_nodes_merged, 0u);

  LabeledGraph value_flow_graph;
  value_flow_graph.addEdge("source", "middle", "indirect");
  value_flow_graph.addEdge("middle", "target", "thread");
  const GraphSimplificationResult value_flow_result = simplifyGraph(
      value_flow_graph, {GraphSimplificationFlavor::ValueFlow, false, true});
  EXPECT_EQ(value_flow_result.graph.vertexCount(), 1u);
  EXPECT_EQ(value_flow_result.statistics.folded_nodes, 2u);
}

TEST(ClassicalArchitectureTest, ValueFlowFoldingPreservesMarkedSources) {
  LabeledGraph graph;
  const NodeId source = graph.addVertex("source");
  const NodeId entry = graph.addVertex("entry");
  graph.markSource(entry);
  graph.addEdge(source, entry, "direct");

  const GraphSimplificationResult result =
      simplifyGraph(graph, {GraphSimplificationFlavor::ValueFlow, false, true});
  EXPECT_EQ(result.graph.vertexCount(), 2u);
  EXPECT_NE(result.representative[source], result.representative[entry]);
  EXPECT_TRUE(result.graph.isSource(result.representative[entry]));
}

TEST(ClassicalArchitectureTest,
     GraphFoldingDoesNotRedetectDirectPairsAfterMerging) {
  LabeledGraph graph;
  graph.addEdge("a", "x", "direct");
  graph.addEdge("a", "b", "direct");
  graph.addEdge("x", "b", "direct");

  const GraphSimplificationResult result =
      simplifyGraph(graph, {GraphSimplificationFlavor::ValueFlow, false, true});
  EXPECT_EQ(result.representative[graph.vertexId("a")],
            result.representative[graph.vertexId("x")]);
  EXPECT_NE(result.representative[graph.vertexId("a")],
            result.representative[graph.vertexId("b")]);
  EXPECT_EQ(result.statistics.folded_nodes, 1u);
}

TEST(ClassicalArchitectureTest, PrunesNonContributingAliasDyckEdges) {
  LabeledGraph graph;
  graph.addEdge("anchor", "left", "d");
  graph.addEdge("left", "anchor", "dbar");
  graph.addEdge("anchor", "right", "d");
  graph.addEdge("right", "anchor", "dbar");
  graph.addEdge("isolated", "leaf", "d");
  graph.addEdge("leaf", "isolated", "dbar");
  graph.addEdge("anchor", "copy", "a");
  graph.addEdge("copy", "anchor", "abar");

  const GraphSimplificationResult result = simplifyGraph(
      graph, {GraphSimplificationFlavor::Alias, false, false, true});
  EXPECT_TRUE(result.graph.hasEdge(graph.vertexId("anchor"),
                                   graph.vertexId("left"), "d"));
  EXPECT_TRUE(result.graph.hasEdge(graph.vertexId("left"),
                                   graph.vertexId("anchor"), "dbar"));
  EXPECT_FALSE(result.graph.hasEdge(graph.vertexId("isolated"),
                                    graph.vertexId("leaf"), "d"));
  EXPECT_TRUE(result.graph.hasEdge(graph.vertexId("anchor"),
                                   graph.vertexId("copy"), "a"));
  EXPECT_GT(result.statistics.interdyck_edges_pruned, 0u);
}

TEST(ClassicalArchitectureTest, PrunesNonContributingValueFlowDyckEdges) {
  LabeledGraph graph;
  graph.addEdge("caller", "inside", "call_1");
  graph.addEdge("inside", "result", "ret_1");
  graph.addEdge("other", "unmatched", "call_2");
  graph.addEdge("inside", "copy", "direct");

  const GraphSimplificationResult result = simplifyGraph(
      graph, {GraphSimplificationFlavor::ValueFlow, false, false, true});
  EXPECT_TRUE(result.graph.hasEdge(graph.vertexId("caller"),
                                   graph.vertexId("inside"), "call_1"));
  EXPECT_TRUE(result.graph.hasEdge(graph.vertexId("inside"),
                                   graph.vertexId("result"), "ret_1"));
  EXPECT_FALSE(result.graph.hasEdge(graph.vertexId("other"),
                                    graph.vertexId("unmatched"), "call_2"));
  EXPECT_TRUE(result.graph.hasEdge(graph.vertexId("inside"),
                                   graph.vertexId("copy"), "direct"));
}

TEST(ClassicalArchitectureTest,
     AliasGraphFoldingPreservesProjectedStartRelation) {
  const Grammar grammar =
      Grammar::parseFromText("Production:\n"
                             "V\tAbar\tV\nV\tV\tA\nV\n"
                             "A\tA\tA\nA\ta\nA\n"
                             "Abar\tAbar\tAbar\nAbar\tabar\nAbar\n"
                             "Count:\nV\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n0", "abar");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n1", "abar");

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar,
                         SolverBackend::SparseBitVector);
  baseline.solve();

  GraphSimplificationResult simplified = simplifyGraph(
      graph, {GraphSimplificationFlavor::Alias, false, true, false});
  SolverSession reduced(simplified.graph, grammar, SolverBackend::Pocr);
  reduced.solve();

  const SymbolId value = grammar.symbolId("V");
  std::set<std::pair<NodeId, NodeId>> expected;
  for (const RelationEdge &edge : baseline.relation().edges(value)) {
    expected.insert({simplified.representative[edge.source],
                     simplified.representative[edge.target]});
  }
  std::set<std::pair<NodeId, NodeId>> actual;
  for (const RelationEdge &edge : reduced.relation().edges(value)) {
    actual.insert({edge.source, edge.target});
  }
  EXPECT_EQ(actual, expected);
}

TEST(ClassicalArchitectureTest,
     SpecializedAliasEnginesPerformHorizontalAndDyckPropagation) {
  LabeledGraph graph;
  for (std::size_t node = 0; node < 6; ++node) {
    graph.addVertex("n" + std::to_string(node));
  }
  graph.addEdge(0, 1, "a");
  graph.addEdge(0, 2, "d");
  graph.addEdge(1, 3, "d");
  graph.addEdge(0, 4, "f_7");
  graph.addEdge(1, 5, "f_7");

  PocrAliasEngine pocr(graph);
  FocrAliasEngine focr(graph);
  EXPECT_THROW(pocr.mayAlias(0, 1), std::logic_error);
  const SpecializedPocrStatistics pocr_stats = pocr.solve();
  const SpecializedPocrStatistics focr_stats = focr.solve();

  EXPECT_TRUE(pocr.assignmentReachable(0, 1));
  EXPECT_TRUE(focr.assignmentReachable(0, 1));
  EXPECT_TRUE(pocr.mayAlias(0, 1));
  EXPECT_TRUE(focr.mayAlias(0, 1));
  EXPECT_TRUE(pocr.mayAlias(2, 3));
  EXPECT_TRUE(focr.mayAlias(2, 3));
  EXPECT_TRUE(pocr.mayAlias(4, 5));
  EXPECT_TRUE(focr.mayAlias(4, 5));
  const auto pocr_pairs = pocr.valuePairs();
  const auto focr_pairs = focr.valuePairs();
  const std::set<std::pair<NodeId, NodeId>> pocr_pair_set(pocr_pairs.begin(),
                                                          pocr_pairs.end());
  const std::set<std::pair<NodeId, NodeId>> focr_pair_set(focr_pairs.begin(),
                                                          focr_pairs.end());
  EXPECT_EQ(pocr_pair_set, focr_pair_set);
  LabeledGraph grammar_graph = graph.transformed(EdgeDirection::Bidirectional);
  const Grammar grammar = Grammar::parseFromText(
      "Production:\n"
      "V\tAbar\tV\nM\tDV\td\nDV\tdbar\tV\nV\tV\tA\n"
      "V\tFV_i\tf_i\nV\tM\nV\nFV_i\tfbar_i\tV\n"
      "A\tA\tA\nA\ta\tM\nA\ta\nA\n"
      "Abar\tAbar\tAbar\nAbar\tM\tabar\nAbar\tabar\nAbar\n"
      "Count:\nV\n",
      inferGrammarAttributes(grammar_graph));
  SolverSession grammar_solver(grammar_graph, grammar,
                               SolverBackend::SparseBitVector);
  grammar_solver.solve();
  std::set<std::pair<NodeId, NodeId>> grammar_pairs;
  for (const RelationEdge &edge :
       grammar_solver.relation().edges(grammar.symbolId("V"))) {
    grammar_pairs.insert({edge.source, edge.target});
  }
  EXPECT_EQ(pocr_pair_set, grammar_pairs);
  EXPECT_GT(pocr_stats.matched_pairs, 0u);
  EXPECT_GT(focr_stats.critical_edges, 0u);
}

TEST(ClassicalArchitectureTest,
     SpecializedValueFlowEnginesPerformNestedVerticalPropagation) {
  LabeledGraph graph;
  for (std::size_t node = 0; node < 6; ++node) {
    graph.addVertex("n" + std::to_string(node));
  }
  graph.addEdge(0, 1, "a");
  graph.addEdge(2, 0, "call_7");
  graph.addEdge(1, 3, "ret_7");
  graph.addEdge(4, 2, "call_9");
  graph.addEdge(3, 5, "ret_9");

  PocrValueFlowEngine pocr(graph);
  FocrValueFlowEngine focr(graph);
  const SpecializedPocrStatistics pocr_stats = pocr.solve();
  const SpecializedPocrStatistics focr_stats = focr.solve();

  EXPECT_TRUE(pocr.hasFlow(0, 1));
  EXPECT_TRUE(focr.hasFlow(0, 1));
  EXPECT_TRUE(pocr.hasFlow(2, 3));
  EXPECT_TRUE(focr.hasFlow(2, 3));
  EXPECT_TRUE(pocr.hasFlow(4, 5));
  EXPECT_TRUE(focr.hasFlow(4, 5));
  const auto pocr_pairs = pocr.flowPairs();
  const auto focr_pairs = focr.flowPairs();
  const std::set<std::pair<NodeId, NodeId>> pocr_pair_set(pocr_pairs.begin(),
                                                          pocr_pairs.end());
  const std::set<std::pair<NodeId, NodeId>> focr_pair_set(focr_pairs.begin(),
                                                          focr_pairs.end());
  EXPECT_EQ(pocr_pair_set, focr_pair_set);
  const Grammar grammar =
      Grammar::parseFromText("Production:\nA\tA\tA\nA\tCA_i\tret_i\nA\ta\nA\n"
                             "CA_i\tcall_i\tA\nCount:\nA\n",
                             inferGrammarAttributes(graph));
  LabeledGraph grammar_graph = graph;
  SolverSession grammar_solver(grammar_graph, grammar,
                               SolverBackend::SparseBitVector);
  grammar_solver.solve();
  std::set<std::pair<NodeId, NodeId>> grammar_pairs;
  for (const RelationEdge &edge :
       grammar_solver.relation().edges(grammar.symbolId("A"))) {
    grammar_pairs.insert({edge.source, edge.target});
  }
  EXPECT_EQ(pocr_pair_set, grammar_pairs);
  EXPECT_GT(pocr_stats.matched_pairs, 0u);
  EXPECT_GT(focr_stats.critical_edges, 0u);
}

TEST(ClassicalArchitectureTest,
     SpecializedFocrEnginesOptionallySimplifyCriticalGraphCycles) {
  LabeledGraph alias_graph;
  alias_graph.addEdge("n0", "n1", "a");
  alias_graph.addEdge("n1", "n2", "a");
  alias_graph.addEdge("n2", "n0", "a");
  FocrAliasEngine alias_without_scc(alias_graph);
  FocrAliasEngine alias_with_scc(alias_graph, true);
  const auto alias_without_stats = alias_without_scc.solve();
  const auto alias_with_stats = alias_with_scc.solve();
  EXPECT_EQ(alias_with_scc.valuePairs(), alias_without_scc.valuePairs());
  EXPECT_EQ(alias_without_stats.cycle_simplifications, 0u);
  EXPECT_GT(alias_with_stats.cycle_simplifications, 0u);

  LabeledGraph value_flow_graph;
  value_flow_graph.addEdge("n0", "n1", "a");
  value_flow_graph.addEdge("n1", "n2", "a");
  value_flow_graph.addEdge("n2", "n0", "a");
  FocrValueFlowEngine value_flow_without_scc(value_flow_graph);
  FocrValueFlowEngine value_flow_with_scc(value_flow_graph, true);
  const auto value_flow_without_stats = value_flow_without_scc.solve();
  const auto value_flow_with_stats = value_flow_with_scc.solve();
  EXPECT_EQ(value_flow_with_scc.flowPairs(),
            value_flow_without_scc.flowPairs());
  EXPECT_EQ(value_flow_without_stats.cycle_simplifications, 0u);
  EXPECT_GT(value_flow_with_stats.cycle_simplifications, 0u);
}

TEST(ClassicalArchitectureTest,
     SpecializedAliasEnginesMatchGrammarSemanticsOnGeneratedGraphs) {
  for (std::size_t seed = 1; seed <= 32; ++seed) {
    LabeledGraph physical;
    for (std::size_t node = 0; node < 5; ++node) {
      physical.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 5; ++source) {
      for (std::size_t target = 0; target < 5; ++target) {
        const std::size_t value = source * 31 + target * 17 + seed * 13;
        if (source != target && value % 11 == 0) {
          physical.addEdge(source, target, "a");
        }
        if (value % 13 == 0) {
          physical.addEdge(source, target, "d");
        }
        if (value % 17 == 0) {
          physical.addEdge(source, target, "f_0");
        }
      }
    }

    PocrAliasEngine pocr(physical);
    FocrAliasEngine focr(physical);
    FocrAliasEngine focr_scc(physical, true);
    pocr.solve();
    focr.solve();
    focr_scc.solve();
    const auto pocr_edges = pocr.valuePairs();
    const auto focr_edges = focr.valuePairs();
    const auto focr_scc_edges = focr_scc.valuePairs();
    const std::set<std::pair<NodeId, NodeId>> pocr_pairs(pocr_edges.begin(),
                                                         pocr_edges.end());
    const std::set<std::pair<NodeId, NodeId>> focr_pairs(focr_edges.begin(),
                                                         focr_edges.end());
    const std::set<std::pair<NodeId, NodeId>> focr_scc_pairs(
        focr_scc_edges.begin(), focr_scc_edges.end());

    LabeledGraph grammar_graph =
        physical.transformed(EdgeDirection::Bidirectional);
    const Grammar grammar =
        buildPocrClientGrammar(PocrClientGrammar::StandardAlias, grammar_graph);
    SolverSession grammar_solver(grammar_graph, grammar,
                                 SolverBackend::SparseBitVector);
    grammar_solver.solve();
    std::set<std::pair<NodeId, NodeId>> grammar_pairs;
    for (const RelationEdge &edge :
         grammar_solver.relation().edges(grammar.symbolId("V"))) {
      grammar_pairs.insert({edge.source, edge.target});
    }
    EXPECT_EQ(pocr_pairs, grammar_pairs) << "seed=" << seed;
    EXPECT_EQ(focr_pairs, grammar_pairs) << "seed=" << seed;
    EXPECT_EQ(focr_scc_pairs, grammar_pairs) << "seed=" << seed;

    const Grammar rewritten = buildPocrClientGrammar(
        PocrClientGrammar::RewrittenAlias, grammar_graph);
    LabeledGraph rewritten_graph = grammar_graph;
    SolverSession rewritten_solver(rewritten_graph, rewritten,
                                   SolverBackend::SparseBitVector);
    rewritten_solver.solve();
    std::set<std::pair<NodeId, NodeId>> rewritten_pairs;
    for (const RelationEdge &edge :
         rewritten_solver.relation().edges(rewritten.startSymbolId())) {
      rewritten_pairs.insert({edge.source, edge.target});
    }
    EXPECT_EQ(rewritten_pairs, grammar_pairs) << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest,
     SpecializedValueFlowEnginesMatchGrammarOnGeneratedGraphs) {
  for (std::size_t seed = 1; seed <= 32; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 5; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 5; ++source) {
      for (std::size_t target = 0; target < 5; ++target) {
        const std::size_t value = source * 19 + target * 29 + seed * 7;
        if (source != target && value % 11 == 0) {
          graph.addEdge(source, target, "a");
        }
        if (value % 17 == 0) {
          graph.addEdge(source, target, "call_0");
        }
        if (value % 19 == 0) {
          graph.addEdge(source, target, "ret_0");
        }
      }
    }

    PocrValueFlowEngine pocr(graph);
    FocrValueFlowEngine focr(graph);
    FocrValueFlowEngine focr_scc(graph, true);
    pocr.solve();
    focr.solve();
    focr_scc.solve();
    const auto pocr_edges = pocr.flowPairs();
    const auto focr_edges = focr.flowPairs();
    const auto focr_scc_edges = focr_scc.flowPairs();
    const std::set<std::pair<NodeId, NodeId>> pocr_pairs(pocr_edges.begin(),
                                                         pocr_edges.end());
    const std::set<std::pair<NodeId, NodeId>> focr_pairs(focr_edges.begin(),
                                                         focr_edges.end());
    const std::set<std::pair<NodeId, NodeId>> focr_scc_pairs(
        focr_scc_edges.begin(), focr_scc_edges.end());

    const Grammar grammar =
        buildPocrClientGrammar(PocrClientGrammar::StandardValueFlow, graph);
    LabeledGraph grammar_graph = graph;
    SolverSession grammar_solver(grammar_graph, grammar,
                                 SolverBackend::SparseBitVector);
    grammar_solver.solve();
    std::set<std::pair<NodeId, NodeId>> grammar_pairs;
    for (const RelationEdge &edge :
         grammar_solver.relation().edges(grammar.symbolId("A"))) {
      grammar_pairs.insert({edge.source, edge.target});
    }
    EXPECT_EQ(pocr_pairs, grammar_pairs) << "seed=" << seed;
    EXPECT_EQ(focr_pairs, grammar_pairs) << "seed=" << seed;
    EXPECT_EQ(focr_scc_pairs, grammar_pairs) << "seed=" << seed;

    const Grammar rewritten =
        buildPocrClientGrammar(PocrClientGrammar::RewrittenValueFlow, graph);
    LabeledGraph rewritten_graph = graph;
    SolverSession rewritten_solver(rewritten_graph, rewritten,
                                   SolverBackend::SparseBitVector);
    rewritten_solver.solve();
    std::set<std::pair<NodeId, NodeId>> rewritten_pairs;
    for (const RelationEdge &edge :
         rewritten_solver.relation().edges(rewritten.startSymbolId())) {
      rewritten_pairs.insert({edge.source, edge.target});
    }
    EXPECT_EQ(rewritten_pairs, grammar_pairs) << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, RejectsOutOfRangeGraphAttributes) {
  LabeledGraph graph;
  graph.addEdge("a", "b", "call_4294967296");
  EXPECT_THROW(inferGrammarAttributes(graph), std::invalid_argument);
}

TEST(ClassicalArchitectureTest,
     SparseBitvectorsMaintainIncrementalTransitiveClosure) {
  IncrementalTransitiveClosure closure(5);
  EXPECT_EQ(closure.addArc(0, 1).size(), 1u);
  const auto through_two = closure.addArc(1, 2);
  EXPECT_NE(std::find(through_two.begin(), through_two.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 2)),
            through_two.end());

  const auto prefixed = closure.addArc(3, 0);
  EXPECT_NE(std::find(prefixed.begin(), prefixed.end(),
                      std::make_pair<std::size_t, std::size_t>(3, 2)),
            prefixed.end());

  const auto cycle = closure.addArc(2, 0);
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 0)),
            cycle.end());
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(2, 1)),
            cycle.end());
  EXPECT_TRUE(closure.hasPath(0, 0));
  EXPECT_GT(closure.statistics().propagated_pairs, 0u);

  closure.ensureNodeCount(6);
  EXPECT_EQ(closure.addArc(5, 0).size(), 3u);
}

TEST(ClassicalArchitectureTest, PocrMaintainsPairedReachabilityTrees) {
  PocrTransitiveClosure closure(5);
  EXPECT_EQ(closure.addArc(0, 1).size(), 1u);
  const auto through_two = closure.addArc(1, 2);
  EXPECT_NE(std::find(through_two.begin(), through_two.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 2)),
            through_two.end());

  closure.addArc(3, 0);
  const auto cycle = closure.addArc(2, 0);
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(0, 0)),
            cycle.end());
  EXPECT_TRUE(closure.hasPath(3, 2));
  EXPECT_GT(closure.statistics().tree_nodes, closure.statistics().tree_roots);
  EXPECT_GT(closure.statistics().traversal_steps, 0u);

  closure.ensureNodeCount(6);
  EXPECT_EQ(closure.addArc(5, 0).size(), 3u);
}

TEST(ClassicalArchitectureTest, FullyOrderedClosureMaintainsCriticalGraph) {
  FullyOrderedTransitiveClosure closure(4, true);
  closure.addArc(0, 1);
  closure.addArc(1, 2);
  EXPECT_TRUE(closure.hasPath(0, 2));
  const auto cycle = closure.addArc(2, 0);
  EXPECT_NE(std::find(cycle.begin(), cycle.end(),
                      std::make_pair<std::size_t, std::size_t>(1, 1)),
            cycle.end());
  EXPECT_TRUE(closure.hasPath(2, 2));
  EXPECT_GT(closure.statistics().critical_edges, 0u);
  EXPECT_GT(closure.statistics().forward_search_steps, 0u);
  EXPECT_GT(closure.statistics().cycle_simplifications, 0u);
}

TEST(ClassicalArchitectureTest,
     OptimizedClosuresAgreeAcrossIncrementalArcSequences) {
  for (std::size_t seed = 1; seed <= 64; ++seed) {
    IncrementalTransitiveClosure reference(10);
    PocrTransitiveClosure pocr(10);
    FullyOrderedTransitiveClosure fully_ordered(10);
    FullyOrderedTransitiveClosure fully_ordered_scc(10, true);
    for (std::size_t step = 0; step < 80; ++step) {
      const NodeId source = (seed * 17 + step * 13) % 10;
      const NodeId target = (seed * 29 + step * step * 7 + 3) % 10;
      reference.addArc(source, target);
      pocr.addArc(source, target);
      fully_ordered.addArc(source, target);
      fully_ordered_scc.addArc(source, target);
    }
    const auto reference_edges = reference.edges();
    const auto pocr_edges = pocr.edges();
    const auto fully_ordered_edges = fully_ordered.edges();
    const auto fully_ordered_scc_edges = fully_ordered_scc.edges();
    const std::set<std::pair<NodeId, NodeId>> expected(reference_edges.begin(),
                                                       reference_edges.end());
    const std::set<std::pair<NodeId, NodeId>> actual_pocr(pocr_edges.begin(),
                                                          pocr_edges.end());
    const std::set<std::pair<NodeId, NodeId>> actual_fully_ordered(
        fully_ordered_edges.begin(), fully_ordered_edges.end());
    const std::set<std::pair<NodeId, NodeId>> actual_fully_ordered_scc(
        fully_ordered_scc_edges.begin(), fully_ordered_scc_edges.end());
    EXPECT_EQ(actual_pocr, expected) << "seed=" << seed;
    EXPECT_EQ(actual_fully_ordered, expected) << "seed=" << seed;
    EXPECT_EQ(actual_fully_ordered_scc, expected) << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, ParsesSolverBackendNames) {
  EXPECT_EQ(parseSolverBackend("graspan"), SolverBackend::Graspan);
  EXPECT_EQ(parseSolverBackend("sqid"), SolverBackend::Sqid);
  EXPECT_EQ(parseSolverBackend("pearl"), SolverBackend::Pearl);
  EXPECT_EQ(parseSolverBackend("pocr"), SolverBackend::Pocr);
  EXPECT_EQ(parseSolverBackend("hpocr"), SolverBackend::HierarchicalPocr);
  EXPECT_EQ(parseSolverBackend("focr"), SolverBackend::FullyOrdered);
  EXPECT_THROW(parseSolverBackend("not-a-solver"), std::invalid_argument);
}

TEST(ClassicalArchitectureTest,
     SqidUsesAdaptiveForwardAndBackwardDifferentialChaining) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> a; B -> b; S -> A B;\n");
  auto relation = createRelation(RelationBackend::SparseBitVectors, 9);
  SqidEngine engine(grammar, *relation, 9);
  const SymbolId a = grammar.symbolId("a");
  const SymbolId b = grammar.symbolId("b");

  engine.addEdge(a, 0, 3);
  engine.addEdge(a, 1, 3);
  engine.addEdge(a, 2, 3);
  engine.addEdge(b, 3, 4);
  engine.addEdge(a, 4, 5);
  engine.addEdge(b, 5, 6);
  engine.addEdge(b, 5, 7);
  engine.addEdge(b, 5, 8);
  const SqidStatistics statistics = engine.solve();

  const SymbolId start = grammar.startSymbolId();
  EXPECT_TRUE(relation->contains(start, 0, 4));
  EXPECT_TRUE(relation->contains(start, 1, 4));
  EXPECT_TRUE(relation->contains(start, 2, 4));
  EXPECT_TRUE(relation->contains(start, 4, 6));
  EXPECT_TRUE(relation->contains(start, 4, 7));
  EXPECT_TRUE(relation->contains(start, 4, 8));
  EXPECT_GT(statistics.backward_chains, 0u);
  EXPECT_GT(statistics.forward_chains, 0u);
  EXPECT_TRUE(engine.empty());
}

TEST(ClassicalArchitectureTest, PaperEnginesInitializeNullableProductions) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> a | <epsilon>;\n");

  auto sqid_relation = createRelation(RelationBackend::SparseBitVectors, 0);
  SqidEngine sqid(grammar, *sqid_relation, 3);
  sqid.solve();
  for (NodeId node = 0; node < 3; ++node) {
    EXPECT_TRUE(sqid_relation->contains(grammar.startSymbolId(), node, node));
  }

  auto pearl_relation = createRelation(RelationBackend::SparseBitVectors, 0);
  PearlEngine pearl(grammar, *pearl_relation, 3);
  pearl.solve();
  for (NodeId node = 0; node < 3; ++node) {
    EXPECT_TRUE(pearl_relation->contains(grammar.startSymbolId(), node, node));
  }
}

TEST(ClassicalArchitectureTest,
     PearlBatchesFullAndBothPartialTransitiveRelations) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a x y\nVariables:\n  S A X Y\n"
      "Productions:\n"
      "  A -> A A | a;\n"
      "  X -> X A | x;\n"
      "  Y -> A Y | y;\n"
      "  S -> X Y;\n");
  auto relation = createRelation(RelationBackend::SparseBitVectors, 5);
  PearlEngine engine(grammar, *relation, 5);
  engine.addEdge(grammar.symbolId("x"), 0, 1);
  engine.addEdge(grammar.symbolId("a"), 1, 2);
  engine.addEdge(grammar.symbolId("a"), 2, 3);
  engine.addEdge(grammar.symbolId("y"), 3, 4);
  const PearlStatistics statistics = engine.solve();

  EXPECT_TRUE(relation->contains(grammar.symbolId("A"), 1, 3));
  EXPECT_TRUE(relation->contains(grammar.symbolId("X"), 0, 3));
  EXPECT_TRUE(relation->contains(grammar.symbolId("Y"), 1, 4));
  EXPECT_TRUE(relation->contains(grammar.symbolId("S"), 0, 4));
  EXPECT_GT(statistics.fully_transitive_primary_edges, 0u);
  EXPECT_GT(statistics.fully_transitive_secondary_edges, 0u);
  EXPECT_GT(statistics.partially_transitive_nodes, 0u);
  EXPECT_GT(statistics.batch_propagations, 0u);
  EXPECT_TRUE(engine.empty());
}

TEST(ClassicalArchitectureTest, PearlPacksExplicitInverseRelations) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  X\nTerminal:\n  a1 a2 x\n"
                             "Variables:\n  X Xbar A1 A2\nProductions:\n"
                             "  A1 -> A1 A1 | a1; A2 -> A2 A2 | a2;\n"
                             "  X -> X A1 | x; Xbar -> Xbar A2;\n");
  auto relation = createRelation(RelationBackend::SparseBitVectors, 4);
  PearlOptions options;
  options.inverse_relations.push_back(
      {grammar.symbolId("X"), grammar.symbolId("Xbar")});
  PearlEngine engine(grammar, *relation, 4, std::move(options));
  engine.addEdge(grammar.symbolId("x"), 2, 1);
  engine.addEdge(grammar.symbolId("a1"), 1, 0);
  engine.addEdge(grammar.symbolId("a2"), 2, 3);
  const PearlStatistics statistics = engine.solve();

  EXPECT_TRUE(relation->contains(grammar.symbolId("X"), 2, 0));
  EXPECT_TRUE(relation->contains(grammar.symbolId("Xbar"), 0, 2));
  EXPECT_TRUE(relation->contains(grammar.symbolId("Xbar"), 0, 3));
  EXPECT_GT(statistics.batch_propagations, 0u);

  LabeledGraph graph;
  for (std::size_t node = 0; node < 4; ++node) {
    graph.addVertex("n" + std::to_string(node));
  }
  graph.addEdge(2, 1, "x");
  graph.addEdge(1, 0, "a1");
  graph.addEdge(2, 3, "a2");
  SolverSession session(
      graph, grammar,
      SolverOptions{SolverBackend::Pearl, false, false, {{"X", "Xbar"}}});
  session.solve();
  EXPECT_TRUE(session.contains(0, 3, "Xbar"));
}

TEST(ClassicalArchitectureTest,
     PearlTreatsTheInverseOfAFullRelationAsFullyTransitive) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  X\nTerminal:\n  a x\nVariables:\n  X A Abar\n"
      "Productions:\n  A -> A A | a; X -> X Abar | x;\n");
  auto relation = createRelation(RelationBackend::SparseBitVectors, 4);
  PearlOptions options;
  options.inverse_relations.push_back(
      {grammar.symbolId("A"), grammar.symbolId("Abar")});
  PearlEngine engine(grammar, *relation, 4, std::move(options));
  engine.addEdge(grammar.symbolId("a"), 0, 1);
  engine.addEdge(grammar.symbolId("a"), 1, 2);
  engine.addEdge(grammar.symbolId("x"), 3, 2);
  const PearlStatistics statistics = engine.solve();

  EXPECT_TRUE(relation->contains(grammar.symbolId("Abar"), 2, 0));
  EXPECT_TRUE(relation->contains(grammar.symbolId("X"), 3, 0));
  EXPECT_GT(statistics.batch_propagations, 0u);
}

TEST(ClassicalArchitectureTest,
     PearlHandlesRelationsThatAreBothFullAndPartialTransitive) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  X\nTerminal:\n  a x\nVariables:\n  A X\n"
      "Productions:\n  A -> A A | a; X -> X X | X A | x;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "x");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");

  EXPECT_EQ(solveWith(SolverBackend::Pearl, graph, grammar),
            solveReference(graph, grammar));
}

TEST(ClassicalArchitectureTest,
     PearlMatchesCubicReferenceAcrossGeneratedTransitiveProblems) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a b x y\n"
                             "Variables:\n  S A B X Y\nProductions:\n"
                             "  A -> A A | a; B -> B B | b;\n"
                             "  X -> X A | B X | x;\n"
                             "  Y -> A Y | Y B | y;\n"
                             "  S -> X Y | Y X;\n");
  for (std::size_t seed = 1; seed <= 128; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 5; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 5; ++source) {
      for (std::size_t target = 0; target < 5; ++target) {
        for (std::size_t label = 0; label < 4; ++label) {
          const std::size_t value =
              source * 31 + target * 17 + label * 13 + seed * 19;
          if (value % 11 == 0) {
            graph.addEdge(
                source, target,
                std::array<const char *, 4>{"a", "b", "x", "y"}[label]);
          }
        }
      }
    }
    EXPECT_EQ(solveWith(SolverBackend::Pearl, graph, grammar),
              solveReference(graph, grammar))
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest,
     StgSolvesRegularKleeneSequencesInSccTopologicalOrder) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  R\nTerminal:\n  a b\nVariables:\n  R\n"
                             "Productions:\n  R -> a R | b;\n");
  auto relation = createRelation(RelationBackend::SparseBitVectors, 4);
  stg::StagedSpecification specification;
  specification.phase_r.push_back(
      {grammar.symbolId("R"),
       {{{{grammar.symbolId("a")}, true}, {{grammar.symbolId("b")}, false}}}});
  stg::StagedSolver solver(grammar, *relation, std::move(specification), 4);
  solver.addEdge(grammar.symbolId("a"), 0, 1);
  solver.addEdge(grammar.symbolId("a"), 1, 0);
  solver.addEdge(grammar.symbolId("a"), 1, 2);
  solver.addEdge(grammar.symbolId("b"), 2, 3);
  const stg::StagedStatistics statistics = solver.solve();

  EXPECT_TRUE(relation->contains(grammar.symbolId("R"), 0, 3));
  EXPECT_TRUE(relation->contains(grammar.symbolId("R"), 1, 3));
  EXPECT_TRUE(relation->contains(grammar.symbolId("R"), 2, 3));
  EXPECT_GT(statistics.ordered_scc_propagations, 0u);
}

TEST(ClassicalArchitectureTest,
     StgHandlesNestedKleeneViaPaperAuxiliaryNonterminal) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  R\nTerminal:\n  a b\nVariables:\n  R C\nProductions:\n"
      "  C -> a ( b ) *; R -> ( C ) *;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "b");
  graph.addEdge("n2", "n1", "b");
  graph.addEdge("n2", "n3", "a");

  auto relation =
      createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
  stg::StagedSpecification specification;
  specification.phase_r = {
      {grammar.symbolId("C"),
       {{{{grammar.symbolId("a")}, false}, {{grammar.symbolId("b")}, true}}}},
      {grammar.symbolId("R"), {{{{grammar.symbolId("C")}, true}}}},
  };
  stg::StagedSolver staged(grammar, *relation, std::move(specification),
                           graph.vertexCount());
  for (const LabeledEdge &edge : graph.edges()) {
    staged.addEdge(grammar.symbolId(edge.label), edge.source, edge.target);
  }
  staged.solve();

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  std::set<std::pair<NodeId, NodeId>> staged_pairs;
  std::set<std::pair<NodeId, NodeId>> baseline_pairs;
  for (const RelationEdge &edge : relation->edges(grammar.startSymbolId())) {
    staged_pairs.insert({edge.source, edge.target});
  }
  for (const RelationEdge &edge :
       baseline.relation().edges(grammar.startSymbolId())) {
    baseline_pairs.insert({edge.source, edge.target});
  }
  EXPECT_EQ(staged_pairs, baseline_pairs);
  EXPECT_TRUE(relation->contains(grammar.startSymbolId(), 0, 3));
}

TEST(ClassicalArchitectureTest,
     StgDyckCfpAndRegularPhaseMatchMonolithicGrammar) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  call_0 ret_0 s\n"
                             "Variables:\n  S Sum\nProductions:\n"
                             "  Sum -> call_0 S ret_0;\n"
                             "  S -> S S | Sum | s | <epsilon>;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "call_0");
  graph.addEdge("n1", "n2", "s");
  graph.addEdge("n2", "n3", "call_0");
  graph.addEdge("n3", "n4", "s");
  graph.addEdge("n4", "n5", "ret_0");
  graph.addEdge("n5", "n6", "s");
  graph.addEdge("n6", "n7", "ret_0");

  auto relation =
      createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
  stg::StagedSpecification specification = stg::decomposeStandardDyck(
      grammar.symbolId("S"), grammar.symbolId("Sum"), {grammar.symbolId("s")},
      {{grammar.symbolId("call_0"), grammar.symbolId("ret_0")}});
  stg::StagedSolver solver(grammar, *relation, std::move(specification),
                           graph.vertexCount());
  for (const LabeledEdge &edge : graph.edges()) {
    solver.addEdge(grammar.symbolId(edge.label), edge.source, edge.target);
  }
  const stg::StagedStatistics statistics = solver.solve();

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  const SymbolId start = grammar.startSymbolId();
  std::set<std::pair<NodeId, NodeId>> staged_pairs;
  std::set<std::pair<NodeId, NodeId>> baseline_pairs;
  for (const RelationEdge &edge : relation->edges(start)) {
    staged_pairs.insert({edge.source, edge.target});
  }
  for (const RelationEdge &edge : baseline.relation().edges(start)) {
    baseline_pairs.insert({edge.source, edge.target});
  }
  EXPECT_EQ(staged_pairs, baseline_pairs);
  EXPECT_TRUE(relation->contains(start, 0, 7));
  EXPECT_GT(statistics.summary_edges, 0u);
  EXPECT_GT(statistics.dyck_path_edges, 0u);
}

TEST(ClassicalArchitectureTest,
     StgExtendedDyckDecompositionMatchesMonolithicGrammar) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  Start\nTerminal:\n  call_0 ret_0 s\n"
                             "Variables:\n  Start P N S Sum\nProductions:\n"
                             "  Start -> P N;\n"
                             "  P -> S P | ret_0 P | <epsilon>;\n"
                             "  N -> S N | call_0 N | <epsilon>;\n"
                             "  Sum -> call_0 S ret_0;\n"
                             "  S -> S S | Sum | s | <epsilon>;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "ret_0");
  graph.addEdge("n1", "n2", "call_0");
  graph.addEdge("n2", "n3", "s");
  graph.addEdge("n3", "n4", "ret_0");
  graph.addEdge("n4", "n5", "call_0");

  auto relation =
      createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
  stg::StagedSpecification specification = stg::decomposeExtendedDyck(
      grammar.symbolId("Start"), grammar.symbolId("Sum"),
      {grammar.symbolId("s")},
      {{grammar.symbolId("call_0"), grammar.symbolId("ret_0")}});
  stg::StagedSolver solver(grammar, *relation, std::move(specification),
                           graph.vertexCount());
  for (const LabeledEdge &edge : graph.edges()) {
    solver.addEdge(grammar.symbolId(edge.label), edge.source, edge.target);
  }
  solver.solve();

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  std::set<std::pair<NodeId, NodeId>> staged_pairs;
  std::set<std::pair<NodeId, NodeId>> baseline_pairs;
  for (const RelationEdge &edge : relation->edges(grammar.symbolId("Start"))) {
    staged_pairs.insert({edge.source, edge.target});
  }
  for (const RelationEdge &edge :
       baseline.relation().edges(grammar.symbolId("Start"))) {
    baseline_pairs.insert({edge.source, edge.target});
  }
  EXPECT_EQ(staged_pairs, baseline_pairs);
  EXPECT_TRUE(relation->contains(grammar.symbolId("Start"), 0, 5));
}

TEST(ClassicalArchitectureTest, StgAliasCfpMatchesMonolithicGrammar) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  X\nTerminal:\n  open close a abar y b\n"
                             "Variables:\n  X A Abar Y B\nProductions:\n"
                             "  A -> a; Abar -> abar; Y -> y; B -> b;\n"
                             "  X -> open ( A ) * Y ( B ) * close;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "open");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n1", "abar");
  graph.addEdge("n2", "n3", "a");
  graph.addEdge("n3", "n2", "abar");
  graph.addEdge("n3", "n4", "y");
  graph.addEdge("n4", "n5", "b");
  graph.addEdge("n5", "n6", "b");
  graph.addEdge("n6", "n7", "close");

  auto relation =
      createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
  stg::StagedSpecification specification = stg::decomposeAliasCfp(
      {grammar.symbolId("X"), grammar.symbolId("open"),
       grammar.symbolId("close"), grammar.symbolId("Abar"),
       grammar.symbolId("Y"), grammar.symbolId("B")},
      {
          {grammar.symbolId("A"), {{{{grammar.symbolId("a")}, false}}}},
          {grammar.symbolId("Abar"), {{{{grammar.symbolId("abar")}, false}}}},
          {grammar.symbolId("Y"), {{{{grammar.symbolId("y")}, false}}}},
          {grammar.symbolId("B"), {{{{grammar.symbolId("b")}, false}}}},
      });
  stg::StagedSolver solver(grammar, *relation, std::move(specification),
                           graph.vertexCount());
  for (const LabeledEdge &edge : graph.edges()) {
    solver.addEdge(grammar.symbolId(edge.label), edge.source, edge.target);
  }
  const stg::StagedStatistics statistics = solver.solve();

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  std::set<std::pair<NodeId, NodeId>> staged_pairs;
  std::set<std::pair<NodeId, NodeId>> baseline_pairs;
  for (const RelationEdge &edge : relation->edges(grammar.startSymbolId())) {
    staged_pairs.insert({edge.source, edge.target});
  }
  for (const RelationEdge &edge :
       baseline.relation().edges(grammar.startSymbolId())) {
    baseline_pairs.insert({edge.source, edge.target});
  }
  EXPECT_EQ(staged_pairs, baseline_pairs);
  EXPECT_TRUE(relation->contains(grammar.startSymbolId(), 0, 7));
  EXPECT_GT(statistics.alias_forward_path_edges, 0u);
  EXPECT_GT(statistics.alias_backward_path_edges, 0u);
}

TEST(ClassicalArchitectureTest,
     StgAliasCfpReprocessesEdgesGeneratedFromNewSummaries) {
  const Grammar grammar =
      Grammar::parseFromText("Start:\n  X\nTerminal:\n  open close abar y b\n"
                             "Variables:\n  X Abar Y B\nProductions:\n"
                             "  Abar -> abar | X; Y -> y | X; B -> b | X;\n"
                             "  X -> open Y ( B ) * close;\n");
  LabeledGraph graph;
  for (std::size_t node = 0; node <= 12; ++node) {
    graph.addVertex("n" + std::to_string(node));
  }

  // The first summary X(0, 3) feeds all three regular Phase-L relations.
  graph.addEdge("n0", "n1", "open");
  graph.addEdge("n1", "n2", "y");
  graph.addEdge("n2", "n3", "close");

  // New B(0, 3) extends Y(5, 0) and creates X(4, 6).
  graph.addEdge("n4", "n5", "open");
  graph.addEdge("n5", "n0", "y");
  graph.addEdge("n3", "n6", "close");

  // New Y(0, 3) is itself a complete forward path and creates X(7, 8).
  graph.addEdge("n7", "n0", "open");
  graph.addEdge("n3", "n8", "close");

  // New Abar(0, 3) extends an existing backward path from 0 to 3 and
  // creates X(12, 11), exercising Algorithm 1 lines 26-29.
  graph.addEdge("n0", "n10", "y");
  graph.addEdge("n10", "n11", "close");
  graph.addEdge("n12", "n3", "open");

  auto relation =
      createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
  stg::StagedSpecification specification = stg::decomposeAliasCfp(
      {grammar.symbolId("X"), grammar.symbolId("open"),
       grammar.symbolId("close"), grammar.symbolId("Abar"),
       grammar.symbolId("Y"), grammar.symbolId("B")},
      {
          {grammar.symbolId("Abar"),
           {{{{grammar.symbolId("abar")}, false}},
            {{{grammar.symbolId("X")}, false}}}},
          {grammar.symbolId("Y"),
           {{{{grammar.symbolId("y")}, false}},
            {{{grammar.symbolId("X")}, false}}}},
          {grammar.symbolId("B"),
           {{{{grammar.symbolId("b")}, false}},
            {{{grammar.symbolId("X")}, false}}}},
      });
  stg::StagedSolver solver(grammar, *relation, std::move(specification),
                           graph.vertexCount());
  for (const LabeledEdge &edge : graph.edges()) {
    solver.addEdge(grammar.symbolId(edge.label), edge.source, edge.target);
  }
  const stg::StagedStatistics statistics = solver.solve();

  const SymbolId summary = grammar.symbolId("X");
  EXPECT_TRUE(relation->contains(summary, 0, 3));
  EXPECT_TRUE(relation->contains(summary, 4, 6));
  EXPECT_TRUE(relation->contains(summary, 7, 8));
  EXPECT_TRUE(relation->contains(summary, 12, 11));
  EXPECT_GT(statistics.phase_l_rounds, 1u);
}

TEST(ClassicalArchitectureTest,
     StgMatchesMonolithicSolvingOnGeneratedCfpGraphs) {
  const Grammar dyck = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  call_0 ret_0 call_1 ret_1 s\n"
      "Variables:\n  S Sum\nProductions:\n"
      "  Sum -> call_0 S ret_0 | call_1 S ret_1;\n"
      "  S -> S S | Sum | s | <epsilon>;\n");
  const std::array<const char *, 5> dyck_labels{"call_0", "ret_0", "call_1",
                                                "ret_1", "s"};
  for (std::size_t seed = 1; seed <= 32; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 5; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 5; ++source) {
      for (std::size_t target = 0; target < 5; ++target) {
        for (std::size_t label = 0; label < dyck_labels.size(); ++label) {
          if ((source * 17 + target * 29 + label * 31 + seed * 13) % 19 == 0) {
            graph.addEdge(source, target, dyck_labels[label]);
          }
        }
      }
    }
    auto relation =
        createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
    stg::StagedSpecification specification = stg::decomposeStandardDyck(
        dyck.symbolId("S"), dyck.symbolId("Sum"), {dyck.symbolId("s")},
        {{dyck.symbolId("call_0"), dyck.symbolId("ret_0")},
         {dyck.symbolId("call_1"), dyck.symbolId("ret_1")}});
    stg::StagedSolver staged(dyck, *relation, std::move(specification),
                             graph.vertexCount());
    for (const LabeledEdge &edge : graph.edges()) {
      staged.addEdge(dyck.symbolId(edge.label), edge.source, edge.target);
    }
    staged.solve();
    LabeledGraph baseline_graph = graph;
    SolverSession baseline(baseline_graph, dyck, SolverBackend::SparseSet);
    baseline.solve();
    std::set<std::pair<NodeId, NodeId>> staged_pairs;
    std::set<std::pair<NodeId, NodeId>> baseline_pairs;
    for (const RelationEdge &edge : relation->edges(dyck.startSymbolId())) {
      staged_pairs.insert({edge.source, edge.target});
    }
    for (const RelationEdge &edge :
         baseline.relation().edges(dyck.startSymbolId())) {
      baseline_pairs.insert({edge.source, edge.target});
    }
    EXPECT_EQ(staged_pairs, baseline_pairs) << "dyck seed=" << seed;
  }

  const Grammar alias =
      Grammar::parseFromText("Start:\n  X\nTerminal:\n  open close a abar y b\n"
                             "Variables:\n  X A Abar Y B\nProductions:\n"
                             "  A -> a; Abar -> abar; Y -> y; B -> b;\n"
                             "  X -> open ( A ) * Y ( B ) * close;\n");
  for (std::size_t seed = 1; seed <= 32; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 5; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 5; ++source) {
      for (std::size_t target = 0; target < 5; ++target) {
        const std::size_t value = source * 23 + target * 37 + seed * 11;
        if (value % 17 == 0) {
          graph.addEdge(source, target, "a");
          graph.addEdge(target, source, "abar");
        }
        if (value % 19 == 0) {
          graph.addEdge(source, target, "open");
        }
        if (value % 23 == 0) {
          graph.addEdge(source, target, "close");
        }
        if (value % 29 == 0) {
          graph.addEdge(source, target, "y");
        }
        if (value % 31 == 0) {
          graph.addEdge(source, target, "b");
        }
      }
    }
    auto relation =
        createRelation(RelationBackend::SparseBitVectors, graph.vertexCount());
    stg::StagedSpecification specification = stg::decomposeAliasCfp(
        {alias.symbolId("X"), alias.symbolId("open"), alias.symbolId("close"),
         alias.symbolId("Abar"), alias.symbolId("Y"), alias.symbolId("B")},
        {
            {alias.symbolId("A"), {{{{alias.symbolId("a")}, false}}}},
            {alias.symbolId("Abar"), {{{{alias.symbolId("abar")}, false}}}},
            {alias.symbolId("Y"), {{{{alias.symbolId("y")}, false}}}},
            {alias.symbolId("B"), {{{{alias.symbolId("b")}, false}}}},
        });
    stg::StagedSolver staged(alias, *relation, std::move(specification),
                             graph.vertexCount());
    for (const LabeledEdge &edge : graph.edges()) {
      staged.addEdge(alias.symbolId(edge.label), edge.source, edge.target);
    }
    staged.solve();
    LabeledGraph baseline_graph = graph;
    SolverSession baseline(baseline_graph, alias, SolverBackend::SparseSet);
    baseline.solve();
    std::set<std::pair<NodeId, NodeId>> staged_pairs;
    std::set<std::pair<NodeId, NodeId>> baseline_pairs;
    for (const RelationEdge &edge : relation->edges(alias.startSymbolId())) {
      staged_pairs.insert({edge.source, edge.target});
    }
    for (const RelationEdge &edge :
         baseline.relation().edges(alias.startSymbolId())) {
      baseline_pairs.insert({edge.source, edge.target});
    }
    EXPECT_EQ(staged_pairs, baseline_pairs) << "alias seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, AllSolverBackendsProduceTheSameClosure) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a | <epsilon>;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");

  const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::SparseBitVector, graph, grammar),
            baseline);
  EXPECT_EQ(solveWith(SolverBackend::Graspan, graph, grammar), baseline);
  EXPECT_EQ(solveWith(SolverBackend::Sqid, graph, grammar), baseline);
  EXPECT_EQ(solveWith(SolverBackend::Pearl, graph, grammar), baseline);
  EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
            baseline);
  EXPECT_EQ(solveWith(SolverBackend::Pocr, graph, grammar), baseline);
  EXPECT_EQ(solveWith(SolverBackend::HierarchicalPocr, graph, grammar),
            baseline);
  EXPECT_EQ(solveWith(SolverBackend::FullyOrdered, graph, grammar), baseline);
  EXPECT_TRUE(baseline.count({"S", 0, 3}) != 0);

  LabeledGraph graspan_graph = graph;
  SolverSession graspan(graspan_graph, grammar, SolverBackend::Graspan);
  const ReachabilityStats graspan_stats = graspan.solve();
  EXPECT_GT(graspan_stats.graspan_epochs, 1u);

  LabeledGraph transitive_graph = graph;
  SolverSession transitive(transitive_graph, grammar,
                           SolverBackend::TransitiveClosure);
  const ReachabilityStats transitive_stats = transitive.solve();
  EXPECT_EQ(transitive_stats.transitive_closure_instances, 1u);
  EXPECT_GT(transitive_stats.transitive_relation_edges, graph.edgeCount());
  EXPECT_GT(transitive_stats.transitive_propagated_pairs, 0u);

  LabeledGraph pocr_graph = graph;
  SolverSession pocr(pocr_graph, grammar, SolverBackend::Pocr);
  const ReachabilityStats pocr_stats = pocr.solve();
  EXPECT_GT(pocr_stats.pocr_tree_nodes, pocr_stats.pocr_tree_roots);
  EXPECT_GT(pocr_stats.pocr_traversal_steps, 0u);

  LabeledGraph fully_ordered_graph = graph;
  SolverSession fully_ordered(fully_ordered_graph, grammar,
                              SolverBackend::FullyOrdered);
  const ReachabilityStats fully_ordered_stats = fully_ordered.solve();
  EXPECT_GT(fully_ordered_stats.fully_ordered_critical_edges, 0u);
  EXPECT_GT(fully_ordered_stats.fully_ordered_reachability_checks, 0u);
}

TEST(ClassicalArchitectureTest,
     GraspanRevisitsOldSourcesForNewMiddleNodeDeltas) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a c\nVariables:\n  S A B C\n"
      "Productions:\n  A -> a; C -> c; B -> C; S -> A B;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "c");

  SolverSession session(graph, grammar, SolverBackend::Graspan);
  const ReachabilityStats stats = session.solve();
  EXPECT_TRUE(session.contains(0, 2, "S"));
  EXPECT_GE(stats.graspan_epochs, 3u);
  EXPECT_EQ(solveWith(SolverBackend::Graspan, graph, grammar),
            solveReference(graph, grammar));
}

TEST(ClassicalArchitectureTest,
     PocrBackendsUseTreeJoinsForLinearRecursiveRules) {
  const Grammar grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b c\nVariables:\n  S A X Y\n"
      "Productions:\n"
      "  A -> A A | a;\n"
      "  X -> X A | b;\n"
      "  Y -> A Y | c;\n"
      "  S -> X Y;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "b");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "a");
  graph.addEdge("n3", "n4", "a");
  graph.addEdge("n4", "n5", "c");

  const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);

  LabeledGraph pocr_graph = graph;
  SolverSession pocr(pocr_graph, grammar, SolverBackend::Pocr);
  const ReachabilityStats pocr_stats = pocr.solve();
  EXPECT_EQ(solveWith(SolverBackend::Pocr, graph, grammar), baseline);
  EXPECT_TRUE(pocr.contains(0, 5, "S"));
  EXPECT_GT(pocr_stats.pocr_tree_join_visits, 0u);

  LabeledGraph focr_graph = graph;
  SolverSession focr(focr_graph, grammar, SolverBackend::FullyOrdered);
  const ReachabilityStats focr_stats = focr.solve();
  EXPECT_EQ(solveWith(SolverBackend::FullyOrdered, graph, grammar), baseline);
  EXPECT_TRUE(focr.contains(0, 5, "S"));
  EXPECT_GT(focr_stats.fully_ordered_tree_join_visits, 0u);

  LabeledGraph focr_scc_graph = graph;
  SolverSession focr_scc(
      focr_scc_graph, grammar,
      SolverOptions{SolverBackend::FullyOrdered, false, true});
  focr_scc.solve();
  std::set<NamedEdge> focr_scc_result;
  for (const RelationEdge &edge : focr_scc.relation().edges()) {
    focr_scc_result.emplace(grammar.symbolName(edge.symbol), edge.source,
                            edge.target);
  }
  EXPECT_EQ(focr_scc_result, baseline);
}

TEST(ClassicalArchitectureTest, BackendsAgreeAcrossGeneratedSmallGraphs) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> a; B -> b; S -> S S | A B | <epsilon>;\n");

  for (std::size_t seed = 1; seed <= 24; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 6; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 6; ++source) {
      for (std::size_t target = 0; target < 6; ++target) {
        const std::size_t value = source * 17 + target * 31 + seed * 13;
        if (value % 11 == 0) {
          graph.addEdge(source, target, "a");
        }
        if (value % 13 == 0) {
          graph.addEdge(source, target, "b");
        }
      }
    }

    const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);
    EXPECT_EQ(solveWith(SolverBackend::SparseBitVector, graph, grammar),
              baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::Graspan, graph, grammar), baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::Sqid, graph, grammar), baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::Pearl, graph, grammar), baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
              baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::Pocr, graph, grammar), baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::HierarchicalPocr, graph, grammar),
              baseline)
        << "seed=" << seed;
    EXPECT_EQ(solveWith(SolverBackend::FullyOrdered, graph, grammar), baseline)
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest,
     AllBackendsMatchCubicReferenceAcrossRandomProblems) {
  for (std::uint64_t seed = 1; seed <= 1000; ++seed) {
    std::uint64_t state = seed;
    auto next = [&]() {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return state;
    };

    std::ostringstream grammar_text;
    grammar_text << "Start:\n  S\nTerminal:\n  a b c\nVariables:\n  S A B C\n"
                    "Productions:\n  A -> a";
    if ((next() & 1U) != 0) {
      grammar_text << " | <epsilon>";
    }
    grammar_text << ";\n  B -> b";
    if ((next() & 1U) != 0) {
      grammar_text << " | A";
    }
    grammar_text << ";\n  C -> c";
    if ((next() & 1U) != 0) {
      grammar_text << " | A B";
    }
    grammar_text << ";\n  S -> A B";
    if ((next() & 1U) != 0) {
      grammar_text << " | B C";
    }
    if ((next() & 1U) != 0) {
      grammar_text << " | C A";
    }
    if ((next() & 1U) != 0) {
      grammar_text << " | S S";
    }
    if ((next() & 1U) != 0) {
      grammar_text << " | <epsilon>";
    }
    grammar_text << ";\n";
    const Grammar grammar = Grammar::parseFromText(grammar_text.str());

    LabeledGraph graph;
    for (std::size_t node = 0; node < 4; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 4; ++source) {
      for (std::size_t target = 0; target < 4; ++target) {
        for (const std::string &label : {"a", "b", "c"}) {
          if (next() % 7 == 0) {
            graph.addEdge(source, target, label);
          }
        }
      }
    }

    const auto reference = solveReference(graph, grammar);
    for (SolverBackend backend :
         {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
          SolverBackend::Graspan, SolverBackend::Sqid, SolverBackend::Pearl,
          SolverBackend::TransitiveClosure, SolverBackend::Pocr,
          SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered}) {
      EXPECT_EQ(solveWith(backend, graph, grammar), reference)
          << "seed=" << seed << " backend=" << solverBackendName(backend);
    }
  }
}

TEST(ClassicalArchitectureTest,
     TransitiveClosureDerivesNonNullableReflexiveCycles) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n0", "a");

  const auto baseline = solveWith(SolverBackend::SparseSet, graph, grammar);
  EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
            baseline);
  EXPECT_EQ(solveWith(SolverBackend::Pocr, graph, grammar), baseline);
  EXPECT_EQ(solveWith(SolverBackend::HierarchicalPocr, graph, grammar),
            baseline);
  EXPECT_EQ(solveWith(SolverBackend::FullyOrdered, graph, grammar), baseline);
  EXPECT_TRUE(baseline.count({"S", 0, 0}) != 0);
  EXPECT_TRUE(baseline.count({"S", 1, 1}) != 0);
}

TEST(ClassicalArchitectureTest,
     TransitiveClosureMatchesSparseSetOnGeneratedCyclicGraphs) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a;\n");
  for (std::size_t seed = 1; seed <= 32; ++seed) {
    LabeledGraph graph;
    for (std::size_t node = 0; node < 7; ++node) {
      graph.addVertex("n" + std::to_string(node));
    }
    for (std::size_t source = 0; source < 7; ++source) {
      for (std::size_t target = 0; target < 7; ++target) {
        if ((source * 19 + target * 23 + seed * 29) % 9 == 0) {
          graph.addEdge(source, target, "a");
        }
      }
    }
    EXPECT_EQ(solveWith(SolverBackend::TransitiveClosure, graph, grammar),
              solveWith(SolverBackend::SparseSet, graph, grammar))
        << "seed=" << seed;
  }
}

TEST(ClassicalArchitectureTest, UsesOneClosurePerTransitiveGrammarSymbol) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S A B\n"
      "Productions:\n  A -> A A | a; B -> B B | b; S -> A B;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  graph.addEdge("n2", "n3", "b");
  graph.addEdge("n3", "n4", "b");

  LabeledGraph baseline_graph = graph;
  SolverSession baseline(baseline_graph, grammar, SolverBackend::SparseSet);
  baseline.solve();
  SolverSession transitive(graph, grammar, SolverBackend::TransitiveClosure);
  const ReachabilityStats stats = transitive.solve();

  EXPECT_TRUE(transitive.contains(0, 4, "S"));
  EXPECT_EQ(stats.transitive_closure_instances, 2u);
  EXPECT_EQ(transitive.relation().edgeCount(), baseline.relation().edgeCount());
}

TEST(ClassicalArchitectureTest, IncrementalSessionResumesToFixedPoint) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a b\nVariables:\n  S\nProductions:\n"
      "  S -> a b;\n");
  LabeledGraph graph;
  const auto n0 = graph.addVertex("n0");
  const auto n1 = graph.addVertex("n1");
  const auto n2 = graph.addVertex("n2");
  graph.addEdge(n0, n1, "a");

  SolverSession session(graph, grammar, SolverBackend::SparseBitVector);
  session.solve();
  EXPECT_FALSE(session.contains(n0, n2, "S"));
  EXPECT_FALSE(graph.hasEdge(n0, n2, "S"));

  EXPECT_TRUE(session.addTerminalEdge(n1, n2, "b"));
  const auto stats = session.solve();
  EXPECT_TRUE(session.contains(n0, n2, "S"));
  EXPECT_FALSE(graph.hasEdge(n0, n2, "S"));
  EXPECT_GT(stats.added_edges, 0u);
}

TEST(ClassicalArchitectureTest,
     OptimizedSessionsResumeTransitiveClosureAfterNewEdges) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> S S | a;\n");
  for (SolverBackend backend :
       {SolverBackend::TransitiveClosure, SolverBackend::Pocr,
        SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered}) {
    LabeledGraph graph;
    const NodeId n0 = graph.addVertex("n0");
    const NodeId n1 = graph.addVertex("n1");
    const NodeId n2 = graph.addVertex("n2");
    graph.addEdge(n0, n1, "a");

    SolverSession session(graph, grammar, backend);
    session.solve();
    EXPECT_FALSE(session.contains(n0, n2, "S")) << solverBackendName(backend);
    EXPECT_TRUE(session.addTerminalEdge(n1, n2, "a"))
        << solverBackendName(backend);
    const ReachabilityStats stats = session.solve();
    EXPECT_TRUE(session.contains(n0, n2, "S")) << solverBackendName(backend);
    EXPECT_GT(stats.transitive_propagated_pairs, 0u)
        << solverBackendName(backend);
  }
}

TEST(ClassicalArchitectureTest, IncrementalSessionSupportsDiscoveredNodes) {
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> a | <epsilon>;\n");
  LabeledGraph graph;
  graph.addVertex("existing");
  SolverSession session(graph, grammar, SolverBackend::SparseBitVector);
  session.solve();

  const auto discovered = session.addNode("discovered");
  session.solve();
  EXPECT_TRUE(session.contains(discovered, discovered, "S"));
}

TEST(ClassicalArchitectureTest, RepeatedSolveDoesNotReseedNullableFacts) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> a | <epsilon>;\n");
  for (SolverBackend backend :
       {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
        SolverBackend::Graspan, SolverBackend::Sqid, SolverBackend::Pearl,
        SolverBackend::TransitiveClosure, SolverBackend::Pocr,
        SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered}) {
    LabeledGraph graph;
    graph.addVertex("n0");
    SolverSession session(graph, grammar, backend);
    session.solve();

    const ReachabilityStats unchanged = session.solve();
    EXPECT_EQ(unchanged.added_edges, 0u) << solverBackendName(backend);
    EXPECT_EQ(unchanged.duplicate_edges, 0u) << solverBackendName(backend);
    EXPECT_EQ(unchanged.processed_work_items, 0u) << solverBackendName(backend);
    EXPECT_EQ(unchanged.peak_worklist_size, 0u) << solverBackendName(backend);
    EXPECT_EQ(unchanged.transitive_arc_insertions, 0u)
        << solverBackendName(backend);
    EXPECT_EQ(unchanged.transitive_propagated_pairs, 0u)
        << solverBackendName(backend);

    const NodeId added = session.addNode("n1");
    const ReachabilityStats incremental = session.solve();
    EXPECT_TRUE(session.contains(added, added, "S"));
    EXPECT_GT(incremental.added_edges, 0u) << solverBackendName(backend);
  }
}

TEST(ClassicalArchitectureTest, DetectsGraphMutationOutsideSession) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addVertex("n2");
  SolverSession session(graph, grammar);
  session.solve();

  graph.addEdge(1, 2, "a");
  EXPECT_THROW(session.solve(), std::logic_error);
  EXPECT_THROW(session.contains(1, 2, "S"), std::logic_error);
}

TEST(ClassicalArchitectureTest, RelationSupportsPerSymbolQueriesAndCounts) {
  const auto grammar =
      Grammar::parseFromText("Start:\n  S\nTerminal:\n  a\nVariables:\n  S\n"
                             "Productions:\n  S -> S S | a;\n");
  LabeledGraph graph;
  graph.addEdge("n0", "n1", "a");
  graph.addEdge("n1", "n2", "a");
  SolverSession session(graph, grammar, SolverBackend::TransitiveClosure);
  session.solve();

  const SymbolId symbol = grammar.symbolId("S");
  EXPECT_EQ(session.relation().edgeCount(symbol),
            session.relation().edges(symbol).size());
  std::size_t successor_count = 0;
  session.relation().forEachSuccessor(symbol, 0,
                                      [&](NodeId) { ++successor_count; });
  EXPECT_EQ(successor_count, 2u);
}

TEST(ClassicalArchitectureTest, SparseBitVectorRejectsUnrepresentableNodes) {
  if constexpr (sizeof(std::size_t) > sizeof(unsigned)) {
    const std::size_t too_many_nodes =
        static_cast<std::size_t>(std::numeric_limits<unsigned>::max()) + 1;
    EXPECT_THROW(
        createRelation(RelationBackend::SparseBitVectors, too_many_nodes),
        std::overflow_error);
  }
}

TEST(ClassicalArchitectureTest, DirectionTransformsAreExplicitAndAttributed) {
  LabeledGraph graph;
  graph.addEdge("caller", "callee", "call_9");
  const auto transformed = graph.transformed(EdgeDirection::Bidirectional);
  EXPECT_TRUE(transformed.hasEdge(0, 1, "call_9"));
  EXPECT_TRUE(transformed.hasEdge(1, 0, "callbar_9"));
  EXPECT_EQ(LabeledGraph::complementLabel("callbar_9"), "call_9");
}

TEST(ClassicalArchitectureTest, BuildsFromClientOwnedRanges) {
  struct Edge {
    std::string source;
    std::string target;
    std::string label;
  };
  const std::vector<std::string> nodes{"left", "right"};
  const std::vector<Edge> edges{{"left", "right", "a"}};
  const auto graph = LabeledGraph::build(
      nodes, edges, [](const std::string &node) { return node; },
      [](const Edge &edge) { return edge.source; },
      [](const Edge &edge) { return edge.target; },
      [](const Edge &edge) { return edge.label; });
  EXPECT_TRUE(graph.hasEdge(0, 1, "a"));
}

TEST(ClassicalArchitectureTest, LoadsJsonGraphAndValidatesLabels) {
  const auto path =
      writeTemp("lotus_classical_graph.json",
                R"({"edges":[{"source":"n0","target":"n1","label":"a"}]})");
  const auto graph = LabeledGraph::parseFromFile(
      path.string(), GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  const auto grammar = Grammar::parseFromText(
      "Start:\n  S\nTerminal:\n  a\nVariables:\n  S\nProductions:\n"
      "  S -> a;\n");

  EXPECT_TRUE(graph.hasEdge(0, 1, "a"));
  EXPECT_TRUE(validateGraph(graph, grammar).empty());

  LabeledGraph invalid_graph = graph;
  invalid_graph.addEdge(0, 1, "unknown");
  const auto issues = validateGraph(invalid_graph, grammar);
  EXPECT_TRUE(
      std::any_of(issues.begin(), issues.end(), [](const GrammarIssue &issue) {
        return issue.severity == GrammarIssueSeverity::Error;
      }));
}

TEST(ClassicalArchitectureTest, WritesNormalizedGraphWithSourceMarkers) {
  LabeledGraph graph;
  const NodeId source = graph.addVertex("source");
  const NodeId target = graph.addVertex("target");
  graph.markSource(source);
  graph.addEdge(source, target, "call_4");
  const auto path =
      std::filesystem::temp_directory_path() / "lotus_normalized_graph.txt";
  graph.writeTextFile(path.string());

  const LabeledGraph loaded = LabeledGraph::parseFromFile(
      path.string(), GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  EXPECT_TRUE(loaded.isSource(loaded.vertexId("source")));
  EXPECT_TRUE(loaded.hasEdge(loaded.vertexId("source"),
                             loaded.vertexId("target"), "call_4"));
}

TEST(ClassicalArchitectureTest,
     JsonAndDotInputsPreserveIsolatedAndQuotedVertices) {
  const auto json_path =
      writeTemp("lotus_classical_isolated.json",
                R"({"nodes":["isolated",{"id":"connected node"}],"edges":[]})");
  const LabeledGraph json = LabeledGraph::parseFromFile(
      json_path.string(),
      GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  EXPECT_EQ(json.vertexCount(), 2u);
  EXPECT_EQ(json.vertexName(0), "isolated");
  const auto nullable = Grammar::parseFromText(
      "Start:\n  S\nVariables:\n  S\nProductions:\n  S -> <epsilon>;\n");
  LabeledGraph nullable_graph = json;
  SolverSession nullable_session(nullable_graph, nullable);
  nullable_session.solve();
  EXPECT_TRUE(nullable_session.contains(0, 0, "S"));
  EXPECT_TRUE(nullable_session.contains(1, 1, "S"));

  const auto dot_path =
      writeTemp("lotus_classical_quoted.dot",
                "digraph G {\n  \"isolated node\";\n  \"source.node\" -> "
                "\"target-node\" [label=\"edge.label\"];\n}\n");
  const LabeledGraph dot = LabeledGraph::parseFromFile(
      dot_path.string(),
      GraphLoadOptions{GraphMode::Plain, EdgeDirection::Plain});
  EXPECT_EQ(dot.vertexCount(), 3u);
  EXPECT_TRUE(dot.hasEdge(dot.vertexId("source.node"),
                          dot.vertexId("target-node"), "edge.label"));
}

} // namespace
} // namespace lotus::cfl::classical
