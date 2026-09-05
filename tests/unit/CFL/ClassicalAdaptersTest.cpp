#include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/NoCtx.h"
#include "CFL/Classical/Clients/Alias/AliasClient.h"
#include "CFL/Classical/Clients/Alias/AserConstraintAdapter.h"
#include "CFL/Classical/Clients/Alias/LLVMAliasAnalysis.h"
#include "CFL/Classical/Clients/ValueFlow/ValueFlowClient.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"
#include "TestUtils/LLVMHelpers.h"

#include <algorithm>
#include <optional>

#include <gtest/gtest.h>

using namespace lotus::analysis;
using namespace lotus::cfl::classical;
using namespace lotus::unittest;

namespace {

struct TestPointsToStorage {
  static void onNewNodeCreation(aser::NodeID) {}
  static bool insert(aser::NodeID, aser::NodeID) { return true; }
};

TEST(ClassicalAdaptersTest, AdaptsTheNativeAserConstraintGraph) {
  aser::ConstraintGraph<aser::NoCtx> source;
  auto *lhs =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  auto *rhs =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  ASSERT_TRUE(source.addConstraints(lhs, rhs, aser::Constraints::copy));

  const AliasConstraintGraph adapted = encodeAserConstraintGraph(source);
  ASSERT_EQ(adapted.nodeNames().size(), 2u);
  ASSERT_EQ(adapted.edges().size(), 1u);
  EXPECT_EQ(adapted.edges().front().kind, AliasConstraintEdgeKind::Copy);
  EXPECT_EQ(adapted.edges().front().source, lhs->getNodeID());
  EXPECT_EQ(adapted.edges().front().target, rhs->getNodeID());

  AliasClient client = makeAliasClient(source);
  EXPECT_TRUE(
      client.graph().hasEdge(lhs->getNodeID(), rhs->getNodeID(), "copy"));

  ASSERT_TRUE(source.addConstraints(lhs, rhs, aser::Constraints::offset));
  const AliasConstraintGraph field_sensitive = encodeAserConstraintGraph(
      source, [](const auto &, const auto &) -> std::optional<std::uint32_t> {
        return 5;
      });
  EXPECT_TRUE(std::any_of(
      field_sensitive.edges().begin(), field_sensitive.edges().end(),
      [](const AliasConstraintEdge &edge) {
        return edge.kind == AliasConstraintEdgeKind::NormalGep &&
               edge.attribute == 5;
      }));
}

TEST(ClassicalAdaptersTest, SynchronizesLiveAserConstraintGrowth) {
  aser::ConstraintGraph<aser::NoCtx> source;
  auto *first =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  auto *second =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  ASSERT_TRUE(source.addConstraints(first, second, aser::Constraints::copy));

  AliasClient client = makeAliasClient(source);
  AserAliasSynchronizer<aser::NoCtx> synchronizer(source, client);
  bool extended = false;
  aser::CGPtrNode<aser::NoCtx> *third = nullptr;
  const auto stats = client.solveToFixedPoint(
      SolverBackend::SparseBitVector, [&](AliasClient &) {
        if (!extended) {
          third = source.addCGNode<aser::CGPtrNode<aser::NoCtx>,
                                   TestPointsToStorage>();
          EXPECT_TRUE(
              source.addConstraints(second, third, aser::Constraints::copy));
          extended = true;
        }
        return synchronizer.synchronize();
      });

  ASSERT_NE(third, nullptr);
  const std::size_t mapped_third = synchronizer.mappedNode(third->getNodeID());
  EXPECT_TRUE(client.graph().hasEdge(
      synchronizer.mappedNode(second->getNodeID()), mapped_third, "copy"));
  EXPECT_EQ(stats.solver_rounds, 2u);
}

TEST(ClassicalAdaptersTest, AserSynchronizerMapsPastPegSyntheticNodes) {
  aser::ConstraintGraph<aser::NoCtx> source;
  auto *pointer =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  auto *value =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  auto *loaded =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  AliasClient client = makeAliasClient(source, AliasEncodingMode::PEG);
  AserAliasSynchronizer<aser::NoCtx> synchronizer(source, client);

  ASSERT_TRUE(source.addConstraints(value, pointer, aser::Constraints::store));
  EXPECT_TRUE(synchronizer.synchronize());
  ASSERT_EQ(client.graph().vertexCount(), 4u);

  auto *late =
      source.addCGNode<aser::CGPtrNode<aser::NoCtx>, TestPointsToStorage>();
  ASSERT_TRUE(source.addConstraints(loaded, late, aser::Constraints::copy));
  EXPECT_TRUE(synchronizer.synchronize());
  EXPECT_EQ(late->getNodeID(), 3u);
  EXPECT_EQ(synchronizer.mappedNode(late->getNodeID()), 4u);
  EXPECT_TRUE(client.graph().hasEdge(
      synchronizer.mappedNode(loaded->getNodeID()), 4u, "copy"));
}

TEST(ClassicalAdaptersTest,
     PagAliasClientEncodesBidirectionalEdgesAndAnswersQueries) {
  AliasConstraintGraph graph;
  const auto obj = graph.addNode("obj");
  const auto ptr = graph.addNode("ptr");
  const auto alias = graph.addNode("alias");

  graph.addEdge(obj, ptr, AliasConstraintEdgeKind::Addr);
  graph.addEdge(ptr, alias, AliasConstraintEdgeKind::Copy);

  AliasClient client = AliasClient::fromConstraintGraph(graph);
  EXPECT_THROW(client.mayAlias(ptr, alias), std::logic_error);
  EXPECT_THROW(client.pointsTo(alias), std::logic_error);
  const auto stats = client.solve();

  EXPECT_TRUE(client.graph().hasEdge(obj, ptr, "addr"));
  EXPECT_TRUE(client.graph().hasEdge(ptr, obj, "addrbar"));
  EXPECT_TRUE(client.mayAlias(ptr, alias));

  const auto pts = client.pointsTo(alias);
  EXPECT_EQ(pts, std::vector<std::size_t>({obj}));
  EXPECT_EQ(std::find(pts.begin(), pts.end(), alias), pts.end());
  EXPECT_GT(stats.added_edges, 0u);
}

TEST(ClassicalAdaptersTest, SpecializedAliasEnginesLowerPagMemoryOperations) {
  AliasConstraintGraph graph;
  const auto slot_object = graph.addNode("slot_object");
  const auto value_object = graph.addNode("value_object");
  const auto pointer = graph.addNode("pointer");
  const auto value = graph.addNode("value");
  const auto loaded = graph.addNode("loaded");
  graph.addEdge(slot_object, pointer, AliasConstraintEdgeKind::Addr);
  graph.addEdge(value_object, value, AliasConstraintEdgeKind::Addr);
  graph.addEdge(value, pointer, AliasConstraintEdgeKind::Store);
  graph.addEdge(pointer, loaded, AliasConstraintEdgeKind::Load);

  for (engines::SpecializedPocrBackend backend :
       {engines::SpecializedPocrBackend::Pocr,
        engines::SpecializedPocrBackend::Focr}) {
    AliasClient client = AliasClient::fromConstraintGraph(graph);
    EXPECT_NO_THROW(client.solveSpecialized(backend));
    EXPECT_TRUE(client.mayAlias(value, loaded));
    EXPECT_EQ(client.pointsTo(loaded),
              std::vector<std::size_t>({value_object}));
  }
}

TEST(ClassicalAdaptersTest, AliasClientResumesAfterIncrementalConstraint) {
  AliasConstraintGraph graph;
  const auto object = graph.addNode("object");
  const auto pointer = graph.addNode("pointer");
  const auto alias = graph.addNode("alias");
  graph.addEdge(object, pointer, AliasConstraintEdgeKind::Addr);

  AliasClient client = AliasClient::fromConstraintGraph(graph);
  bool discovered = false;
  const auto stats = client.solveToFixedPoint(
      SolverBackend::SparseBitVector, [&](AliasClient &current) {
        if (discovered) {
          return false;
        }
        EXPECT_FALSE(current.mayAlias(pointer, alias));
        discovered = true;
        return current.addConstraint(pointer, alias,
                                     AliasConstraintEdgeKind::Copy);
      });
  EXPECT_TRUE(client.mayAlias(pointer, alias));
  EXPECT_EQ(stats.solver_rounds, 2u);
}

TEST(ClassicalAdaptersTest,
     PegEncodingRewritesLoadsAndStoresThroughSyntheticDeref) {
  AliasConstraintGraph graph;
  const auto obj = graph.addNode("obj");
  const auto value_obj = graph.addNode("value_obj");
  const auto slot = graph.addNode("slot");
  const auto value = graph.addNode("value");
  const auto loaded = graph.addNode("loaded");

  graph.addEdge(obj, slot, AliasConstraintEdgeKind::Addr);
  graph.addEdge(value_obj, value, AliasConstraintEdgeKind::Addr);
  graph.addEdge(value, slot, AliasConstraintEdgeKind::Store);
  graph.addEdge(slot, loaded, AliasConstraintEdgeKind::Load);

  AliasClient client =
      AliasClient::fromConstraintGraph(graph, AliasEncodingMode::PEG);
  client.solve();

  EXPECT_TRUE(client.mayAlias(value, loaded));
  const auto pts = client.pointsTo(loaded);
  EXPECT_EQ(pts, std::vector<std::size_t>({value_obj}));

  for (AliasEncodingMode mode :
       {AliasEncodingMode::PAG, AliasEncodingMode::PEG}) {
    for (SolverBackend backend :
         {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
          SolverBackend::Graspan, SolverBackend::Sqid, SolverBackend::Pearl,
          SolverBackend::TransitiveClosure, SolverBackend::Pocr,
          SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered,
          SolverBackend::EndpointQuotient}) {
      AliasClient alternate = AliasClient::fromConstraintGraph(graph, mode);
      alternate.solve(backend);
      EXPECT_TRUE(alternate.mayAlias(value, loaded))
          << solverBackendName(backend);
      EXPECT_EQ(alternate.pointsTo(loaded),
                std::vector<std::size_t>({value_obj}))
          << solverBackendName(backend);
    }
  }

  for (engines::SpecializedPocrBackend backend :
       {engines::SpecializedPocrBackend::Pocr,
        engines::SpecializedPocrBackend::Focr}) {
    AliasClient specialized =
        AliasClient::fromConstraintGraph(graph, AliasEncodingMode::PEG);
    const ReachabilityStats specialized_stats =
        specialized.solveSpecialized(backend);
    EXPECT_TRUE(specialized.mayAlias(value, loaded));
    EXPECT_GT(specialized_stats.specialized_reachability_pairs, 0u);
  }
}

TEST(ClassicalAdaptersTest, GepAliasesUseCumulativeAndUnknownOffsets) {
  for (AliasEncodingMode mode :
       {AliasEncodingMode::PAG, AliasEncodingMode::PEG}) {
    AliasConstraintGraph graph;
    const auto object = graph.addNode("object");
    const auto base = graph.addNode("base");
    const auto plus_four = graph.addNode("plus_four");
    const auto chained = graph.addNode("chained");
    const auto plus_eight = graph.addNode("plus_eight");
    const auto unknown = graph.addNode("unknown");
    graph.addEdge(object, base, AliasConstraintEdgeKind::Addr);
    graph.addEdge(base, plus_four, AliasConstraintEdgeKind::NormalGep, 4);
    graph.addEdge(plus_four, chained, AliasConstraintEdgeKind::NormalGep, 4);
    graph.addEdge(base, plus_eight, AliasConstraintEdgeKind::NormalGep, 8);
    graph.addEdge(base, unknown, AliasConstraintEdgeKind::VariantGep);

    AliasClient client = AliasClient::fromConstraintGraph(graph, mode);
    client.solve(SolverBackend::SparseBitVector);
    EXPECT_TRUE(client.mayAlias(chained, plus_eight));
    EXPECT_TRUE(client.mayAlias(plus_eight, chained));
    EXPECT_TRUE(client.mayAlias(unknown, plus_eight));
    EXPECT_TRUE(client.mayAlias(plus_eight, unknown));

    const auto fields = client.pointsTo(chained);
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_NE(fields.front(), base);
    EXPECT_EQ(client.baseObject(fields.front()), object);
  }
}

TEST(ClassicalAdaptersTest, ZeroGepAliasIsSymmetric) {
  for (AliasEncodingMode mode :
       {AliasEncodingMode::PAG, AliasEncodingMode::PEG}) {
    AliasConstraintGraph graph;
    const auto object = graph.addNode("object");
    const auto pointer = graph.addNode("pointer");
    const auto first = graph.addNode("first");
    const auto second = graph.addNode("second");
    const auto peer = graph.addNode("peer");
    graph.addEdge(object, pointer, AliasConstraintEdgeKind::Addr);
    graph.addEdge(pointer, first, AliasConstraintEdgeKind::NormalGep, 0);
    graph.addEdge(first, second, AliasConstraintEdgeKind::NormalGep, 0);
    graph.addEdge(pointer, peer, AliasConstraintEdgeKind::NormalGep, 0);

    LabeledGraph encoded = mode == AliasEncodingMode::PAG
                               ? encodePagGraph(graph)
                               : encodePegGraph(graph);
    const Grammar grammar = mode == AliasEncodingMode::PAG
                                ? buildPagGrammar(graph)
                                : buildPegGrammar(graph);
    SolverSession raw_solver(encoded, grammar, SolverBackend::SparseBitVector);
    raw_solver.solve();
    EXPECT_TRUE(raw_solver.contains(second, peer, "V"));
    EXPECT_TRUE(raw_solver.contains(peer, second, "V"));

    AliasClient client = AliasClient::fromConstraintGraph(graph, mode);
    client.solve(SolverBackend::SparseBitVector);
    EXPECT_TRUE(client.mayAlias(second, peer));
    EXPECT_TRUE(client.mayAlias(peer, second));
  }
}

TEST(ClassicalAdaptersTest, VariantGepRetainsKnownAlignmentClass) {
  AliasConstraintGraph graph;
  const auto object = graph.addNode("object");
  const auto base = graph.addNode("base");
  const auto array_element = graph.addNode("array_element");
  const auto aligned_field = graph.addNode("aligned_field");
  const auto incompatible_field = graph.addNode("incompatible_field");
  graph.addEdge(object, base, AliasConstraintEdgeKind::Addr);
  graph.addEdge(base, array_element, AliasConstraintEdgeKind::VariantGep, 0,
                16);
  graph.addEdge(base, aligned_field, AliasConstraintEdgeKind::NormalGep, 16);
  graph.addEdge(base, incompatible_field, AliasConstraintEdgeKind::NormalGep,
                8);

  AliasClient client = AliasClient::fromConstraintGraph(graph);
  client.solve(SolverBackend::SparseBitVector);
  EXPECT_TRUE(client.mayAlias(array_element, aligned_field));
  EXPECT_FALSE(client.mayAlias(array_element, incompatible_field));
}

TEST(ClassicalAdaptersTest, IncrementalPegConvertsLoadsAndStores) {
  AliasConstraintGraph graph;
  const auto object = graph.addNode("object");
  const auto pointer = graph.addNode("pointer");
  const auto value = graph.addNode("value");
  const auto loaded = graph.addNode("loaded");
  graph.addEdge(object, pointer, AliasConstraintEdgeKind::Addr);

  AliasClient client =
      AliasClient::fromConstraintGraph(graph, AliasEncodingMode::PEG);
  client.solve(SolverBackend::SparseBitVector);
  EXPECT_TRUE(
      client.addConstraint(value, pointer, AliasConstraintEdgeKind::Store));
  EXPECT_TRUE(
      client.addConstraint(pointer, loaded, AliasConstraintEdgeKind::Load));
  client.solve(SolverBackend::SparseBitVector);

  EXPECT_TRUE(client.graph().hasEdge(value, object, "copy"));
  EXPECT_TRUE(client.graph().hasEdge(object, loaded, "copy"));
  EXPECT_TRUE(client.mayAlias(value, loaded));
}

TEST(ClassicalAdaptersTest, IncrementalPegReusesSyntheticDereferenceNode) {
  AliasConstraintGraph graph;
  const auto pointer = graph.addNode("pointer");
  const auto value = graph.addNode("value");
  const auto loaded = graph.addNode("loaded");

  AliasClient client =
      AliasClient::fromConstraintGraph(graph, AliasEncodingMode::PEG);
  client.solve(SolverBackend::TransitiveClosure);
  EXPECT_TRUE(
      client.addConstraint(value, pointer, AliasConstraintEdgeKind::Store));
  EXPECT_TRUE(
      client.addConstraint(pointer, loaded, AliasConstraintEdgeKind::Load));
  client.solve(SolverBackend::TransitiveClosure);

  ASSERT_EQ(client.graph().vertexCount(), 4u);
  const std::size_t dereference = 3;
  EXPECT_TRUE(client.graph().hasEdge(dereference, pointer, "addr"));
  EXPECT_TRUE(client.graph().hasEdge(value, dereference, "copy"));
  EXPECT_TRUE(client.graph().hasEdge(dereference, loaded, "copy"));
  EXPECT_TRUE(client.mayAlias(value, loaded));
}

TEST(ClassicalAdaptersTest, BatchPegReusesSyntheticDereferenceNode) {
  AliasConstraintGraph graph;
  const auto pointer = graph.addNode("pointer");
  const auto value = graph.addNode("value");
  const auto loaded = graph.addNode("loaded");
  graph.addEdge(value, pointer, AliasConstraintEdgeKind::Store);
  graph.addEdge(pointer, loaded, AliasConstraintEdgeKind::Load);

  AliasClient client =
      AliasClient::fromConstraintGraph(graph, AliasEncodingMode::PEG);
  client.solve(SolverBackend::SparseBitVector);

  ASSERT_EQ(client.graph().vertexCount(), 4u);
  const std::size_t dereference = 3;
  EXPECT_TRUE(client.graph().hasEdge(dereference, pointer, "addr"));
  EXPECT_TRUE(client.graph().hasEdge(value, dereference, "copy"));
  EXPECT_TRUE(client.graph().hasEdge(dereference, loaded, "copy"));
  EXPECT_TRUE(client.mayAlias(value, loaded));
}

TEST(ClassicalAdaptersTest, PegResultsAreIndependentOfConstraintOrder) {
  constexpr std::size_t object = 0;
  constexpr std::size_t pointer = 1;
  constexpr std::size_t value = 2;
  constexpr std::size_t loaded = 3;

  auto makeEmptyGraph = [] {
    AliasConstraintGraph graph;
    graph.addNode("object");
    graph.addNode("pointer");
    graph.addNode("value");
    graph.addNode("loaded");
    return graph;
  };
  auto projectedPointsTo = [](const AliasClient &client) {
    std::set<std::size_t> projected;
    for (std::size_t node : client.pointsTo(loaded)) {
      if (node <= loaded) {
        projected.insert(node);
      }
    }
    return projected;
  };

  for (SolverBackend backend :
       {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
        SolverBackend::Graspan, SolverBackend::Sqid, SolverBackend::Pearl,
        SolverBackend::TransitiveClosure, SolverBackend::Pocr,
        SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered}) {
    AliasConstraintGraph batch_graph = makeEmptyGraph();
    batch_graph.addEdge(object, pointer, AliasConstraintEdgeKind::Addr);
    batch_graph.addEdge(value, pointer, AliasConstraintEdgeKind::Store);
    batch_graph.addEdge(pointer, loaded, AliasConstraintEdgeKind::Load);
    AliasClient batch =
        AliasClient::fromConstraintGraph(batch_graph, AliasEncodingMode::PEG);
    batch.solve(backend);

    AliasClient store_first = AliasClient::fromConstraintGraph(
        makeEmptyGraph(), AliasEncodingMode::PEG);
    store_first.solve(backend);
    store_first.addConstraint(value, pointer, AliasConstraintEdgeKind::Store);
    store_first.addConstraint(pointer, loaded, AliasConstraintEdgeKind::Load);
    store_first.addConstraint(object, pointer, AliasConstraintEdgeKind::Addr);
    store_first.solve(backend);

    AliasClient load_first = AliasClient::fromConstraintGraph(
        makeEmptyGraph(), AliasEncodingMode::PEG);
    load_first.solve(backend);
    load_first.addConstraint(pointer, loaded, AliasConstraintEdgeKind::Load);
    load_first.addConstraint(object, pointer, AliasConstraintEdgeKind::Addr);
    load_first.addConstraint(value, pointer, AliasConstraintEdgeKind::Store);
    load_first.solve(backend);

    EXPECT_EQ(store_first.mayAlias(value, loaded),
              batch.mayAlias(value, loaded))
        << solverBackendName(backend);
    EXPECT_EQ(load_first.mayAlias(value, loaded), batch.mayAlias(value, loaded))
        << solverBackendName(backend);
    EXPECT_EQ(projectedPointsTo(store_first), projectedPointsTo(batch))
        << solverBackendName(backend);
    EXPECT_EQ(projectedPointsTo(load_first), projectedPointsTo(batch))
        << solverBackendName(backend);
  }
}

TEST(ClassicalAdaptersTest, GrammarBuildersMaterializeObservedAttributes) {
  AliasConstraintGraph graph;
  const auto base = graph.addNode("base");
  const auto field = graph.addNode("field");
  graph.addEdge(base, field, AliasConstraintEdgeKind::NormalGep, 3);

  const auto pag = buildPagGrammar(graph);
  const auto peg = buildPegGrammar(graph);

  EXPECT_NE(pag.binaryByFirst().find("Gepbar_3"), pag.binaryByFirst().end());
  EXPECT_NE(peg.binaryByFirst().find("gepbar_3"), peg.binaryByFirst().end());
}

TEST(ClassicalAdaptersTest, MovingAliasClientPreservesSolvedSession) {
  AliasConstraintGraph graph;
  const auto object = graph.addNode("object");
  const auto pointer = graph.addNode("pointer");
  const auto alias = graph.addNode("alias");
  graph.addEdge(object, pointer, AliasConstraintEdgeKind::Addr);
  graph.addEdge(pointer, alias, AliasConstraintEdgeKind::Copy);

  AliasClient source = AliasClient::fromConstraintGraph(graph);
  source.solve(SolverBackend::SparseBitVector);
  AliasClient moved(std::move(source));
  EXPECT_TRUE(moved.mayAlias(pointer, alias));
  EXPECT_THROW(moved.solve(SolverBackend::TransitiveClosure),
               std::invalid_argument);

  AliasConstraintGraph empty_graph;
  empty_graph.addNode("unused");
  AliasClient assigned = AliasClient::fromConstraintGraph(empty_graph);
  assigned = std::move(moved);
  EXPECT_TRUE(assigned.mayAlias(pointer, alias));
  EXPECT_NO_THROW(assigned.solve(SolverBackend::SparseBitVector));
}

TEST(ClassicalAdaptersTest, IncrementalGepPreservesExistingClosure) {
  AliasConstraintGraph graph;
  const auto object = graph.addNode("object");
  const auto pointer = graph.addNode("pointer");
  const auto alias = graph.addNode("alias");
  const auto field = graph.addNode("field");
  graph.addEdge(object, pointer, AliasConstraintEdgeKind::Addr);
  graph.addEdge(pointer, alias, AliasConstraintEdgeKind::Copy);

  AliasClient client = AliasClient::fromConstraintGraph(graph);
  client.solve(SolverBackend::SparseBitVector);
  ASSERT_TRUE(client.mayAlias(pointer, alias));
  ASSERT_TRUE(client.addConstraint(alias, field,
                                   AliasConstraintEdgeKind::NormalGep, 17));

  EXPECT_TRUE(client.mayAlias(pointer, alias));
  EXPECT_THROW(client.solve(SolverBackend::TransitiveClosure),
               std::invalid_argument);
  EXPECT_NO_THROW(client.solve(SolverBackend::SparseBitVector));
}

TEST(ClassicalAdaptersTest, IncrementalGepExtendsAttributedGrammar) {
  AliasConstraintGraph graph;
  const auto base = graph.addNode("base");
  const auto field = graph.addNode("field");
  AliasClient client = AliasClient::fromConstraintGraph(graph);
  client.solve(SolverBackend::SparseBitVector);

  EXPECT_TRUE(
      client.addConstraint(base, field, AliasConstraintEdgeKind::NormalGep, 9));
  EXPECT_TRUE(client.grammar().isTerminal("gep_9"));
  EXPECT_TRUE(client.grammar().isTerminal("gepbar_9"));
  client.solve(SolverBackend::SparseBitVector);
  EXPECT_TRUE(client.graph().hasEdge(base, field, "gep_9"));
}

TEST(ClassicalAdaptersTest, ValueFlowClientEncodesSvfgCallsAndReachability) {
  const char *source = R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @caller(i32 %x) {
    entry:
      %r = call i32 @callee(i32 %x)
      ret i32 %r
    }
  )";

  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  SVFG svfg;
  const auto *caller = module->getFunction("caller");
  const auto *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  const llvm::CallBase *callsite = nullptr;
  for (const llvm::Instruction &inst : llvm::instructions(*caller)) {
    if (const auto *cb = llvm::dyn_cast<llvm::CallBase>(&inst)) {
      callsite = cb;
      break;
    }
  }
  ASSERT_NE(callsite, nullptr);

  auto *actual = new ActualParmSVFGNode(1, nullptr, callsite, 0);
  auto *formal = new FormalParmSVFGNode(2, nullptr, callee, 0);
  auto *formal_ret = new FormalRetSVFGNode(3, nullptr, callee);
  auto *actual_ret = new ActualRetSVFGNode(4, nullptr, callsite);
  auto *copy = new CopySVFGNode(5, nullptr, nullptr);
  auto *thread_target = new CopySVFGNode(6, nullptr, nullptr);

  actual->setValueId(10);
  formal->setValueId(11);
  formal_ret->setValueId(12);
  actual_ret->setValueId(13);
  copy->setValueId(14);
  thread_target->setValueId(15);

  svfg.addNode(actual);
  svfg.addActualParm(callsite, actual);
  svfg.addNode(formal);
  svfg.addFormalParm(callee, formal);
  svfg.addNode(formal_ret);
  svfg.addFormalRet(callee, formal_ret);
  svfg.addNode(actual_ret);
  svfg.addActualRet(callsite, actual_ret);
  svfg.addNode(copy);
  svfg.addNode(thread_target);
  svfg.addEdge(actual, formal, SVFGEdgeK::CallInd, callsite);
  svfg.addEdge(formal, copy, SVFGEdgeK::IntraCopy);
  svfg.addEdge(copy, formal_ret, SVFGEdgeK::IntraIndirect, nullptr, {42});
  svfg.addEdge(copy, thread_target, SVFGEdgeK::ThreadMHPIndirectVF, nullptr,
               {42});
  svfg.addEdge(formal_ret, actual_ret, SVFGEdgeK::RetInd, callsite);

  ValueFlowClient client = ValueFlowClient::fromSVFG(svfg);
  EXPECT_THROW(client.hasFlow(1, 4), std::logic_error);
  EXPECT_THROW(client.reachableFrom(1), std::logic_error);
  const auto stats = client.solve();

  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("1"),
                                     client.graph().vertexId("2"), "call_1"));
  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("4"),
                                     client.graph().vertexId("3"), "retbar_1"));
  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("2"),
                                     client.graph().vertexId("5"), "direct"));
  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("5"),
                                     client.graph().vertexId("3"), "indirect"));
  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("5"),
                                     client.graph().vertexId("6"), "thread"));
  EXPECT_TRUE(client.hasFlow(1, 4));
  EXPECT_FALSE(client.hasBalancedFlow(1, 2));
  EXPECT_TRUE(client.hasRealizableFlow(1, 2));
  EXPECT_FALSE(client.hasBalancedFlow(3, 4));
  EXPECT_TRUE(client.hasRealizableFlow(3, 4));
  const auto realizable = client.realizableReachableFrom(1);
  EXPECT_NE(std::find(realizable.begin(), realizable.end(), 2),
            realizable.end());

  const auto reachable = client.reachableFrom(1);
  EXPECT_NE(std::find(reachable.begin(), reachable.end(), 4), reachable.end());
  EXPECT_GT(stats.added_edges, 0u);

  for (SolverBackend backend :
       {SolverBackend::SparseBitVector, SolverBackend::Graspan,
        SolverBackend::Sqid, SolverBackend::Pearl,
        SolverBackend::TransitiveClosure, SolverBackend::Pocr,
        SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered}) {
    ValueFlowClient alternate = ValueFlowClient::fromSVFG(svfg);
    alternate.solve(backend);
    EXPECT_TRUE(alternate.hasFlow(1, 4)) << solverBackendName(backend);
  }
  for (engines::SpecializedPocrBackend backend :
       {engines::SpecializedPocrBackend::Pocr,
        engines::SpecializedPocrBackend::Focr}) {
    ValueFlowClient alternate = ValueFlowClient::fromSVFG(svfg);
    const ReachabilityStats specialized_stats =
        alternate.solveSpecialized(backend);
    EXPECT_TRUE(alternate.hasFlow(1, 4));
    EXPECT_THROW(alternate.hasRealizableFlow(1, 2), std::logic_error);
    EXPECT_GT(specialized_stats.specialized_reachability_pairs, 0u);
  }
}

TEST(ClassicalAdaptersTest, SvfgCallsiteIdsFollowStableInstructionOrder) {
  const char *source = R"(
    define i32 @callee(i32 %x) {
    entry:
      ret i32 %x
    }

    define i32 @caller(i32 %x) {
    entry:
      %first = call i32 @callee(i32 %x)
      %second = call i32 @callee(i32 %first)
      ret i32 %second
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);
  const auto *caller = module->getFunction("caller");
  const auto *callee = module->getFunction("callee");
  ASSERT_NE(caller, nullptr);
  ASSERT_NE(callee, nullptr);

  std::vector<const llvm::CallBase *> calls;
  for (const llvm::Instruction &instruction : llvm::instructions(*caller)) {
    if (const auto *call = llvm::dyn_cast<llvm::CallBase>(&instruction)) {
      calls.push_back(call);
    }
  }
  ASSERT_EQ(calls.size(), 2u);

  SVFG svfg;
  auto *first = new ActualParmSVFGNode(1, nullptr, calls[0], 0);
  auto *second = new ActualParmSVFGNode(2, nullptr, calls[1], 0);
  auto *formal = new FormalParmSVFGNode(3, nullptr, callee, 0);
  svfg.addNode(first);
  svfg.addNode(second);
  svfg.addNode(formal);
  svfg.addEdge(first, formal, SVFGEdgeK::CallInd, calls[0]);
  svfg.addEdge(second, formal, SVFGEdgeK::CallInd, calls[1]);

  const LabeledGraph encoded = encodeSVFG(svfg);
  EXPECT_TRUE(
      encoded.hasEdge(encoded.vertexId("1"), encoded.vertexId("3"), "call_1"));
  EXPECT_TRUE(
      encoded.hasEdge(encoded.vertexId("2"), encoded.vertexId("3"), "call_2"));
  const Grammar grammar = buildVfgGrammar(svfg);
  EXPECT_TRUE(grammar.isTerminal("call_1"));
  EXPECT_TRUE(grammar.isTerminal("call_2"));
}

TEST(ClassicalAdaptersTest, MovingValueFlowClientPreservesSolvedSession) {
  SVFG svfg;
  auto *source = new CopySVFGNode(1, nullptr, nullptr);
  auto *target = new CopySVFGNode(2, nullptr, nullptr);
  svfg.addNode(source);
  svfg.addNode(target);
  svfg.addEdge(source, target, SVFGEdgeK::IntraCopy);

  ValueFlowClient original = ValueFlowClient::fromSVFG(svfg);
  original.solve(SolverBackend::SparseBitVector);
  ValueFlowClient moved(std::move(original));
  EXPECT_TRUE(moved.hasFlow(1, 2));
  EXPECT_THROW(moved.solve(SolverBackend::TransitiveClosure),
               std::invalid_argument);

  SVFG empty_svfg;
  empty_svfg.addNode(new CopySVFGNode(3, nullptr, nullptr));
  ValueFlowClient assigned = ValueFlowClient::fromSVFG(empty_svfg);
  assigned = std::move(moved);
  EXPECT_TRUE(assigned.hasFlow(1, 2));
  EXPECT_NO_THROW(assigned.solve(SolverBackend::SparseBitVector));
}

TEST(ClassicalAdaptersTest, ValueFlowEncodingRejectsUnsupportedEdges) {
  SVFG svfg;
  auto *source = new CopySVFGNode(1, nullptr, nullptr);
  auto *target = new CopySVFGNode(2, nullptr, nullptr);
  svfg.addNode(source);
  svfg.addNode(target);
  svfg.addEdge(source, target, SVFGEdgeK::Variant);

  EXPECT_THROW(encodeSVFG(svfg), std::invalid_argument);
}

TEST(ClassicalAdaptersTest, SvfgPreparationRequiresPositiveSingletonEvidence) {
  SVFG svfg;
  auto *pointer = new CopySVFGNode(1, nullptr, nullptr);
  auto *previous = new CopySVFGNode(2, nullptr, nullptr);
  auto *store = new StoreSVFGNode(3, nullptr, nullptr, 1);
  store->setMemoryDef(7, 1, {42});
  svfg.addNode(pointer);
  svfg.addNode(previous);
  svfg.addNode(store);
  svfg.addEdge(pointer, store, SVFGEdgeK::IntraStore);
  svfg.addEdge(previous, store, SVFGEdgeK::IntraIndirect, nullptr, {42});
  SVFG::ObjectInfo info;
  info.isGlobal = true;
  svfg.setObjectInfo(42, info);

  const SVFGPreparationStatistics stats = prepareSVFGForCFL(svfg);
  EXPECT_EQ(stats.stores_examined, 1u);
  EXPECT_EQ(stats.strong_update_stores, 0u);
  EXPECT_EQ(stats.dereference_edges_removed, 1u);
  EXPECT_EQ(stats.strong_update_edges_removed, 0u);
  ASSERT_EQ(store->getInEdges().size(), 1u);
  EXPECT_TRUE(store->getInEdges().front()->isIndirectEdge());
  EXPECT_NO_THROW(encodeSVFG(svfg));
}

TEST(ClassicalAdaptersTest, SvfgPreparationPreservesWeakUpdateFlow) {
  SVFG svfg;
  auto *previous = new CopySVFGNode(1, nullptr, nullptr);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 3);
  store->setMemoryDef(7, 1, {42, 43});
  svfg.addNode(previous);
  svfg.addNode(store);
  svfg.addEdge(previous, store, SVFGEdgeK::IntraIndirect, nullptr, {42});

  const SVFGPreparationStatistics stats = prepareSVFGForCFL(svfg);
  EXPECT_EQ(stats.strong_update_stores, 0u);
  EXPECT_EQ(stats.strong_update_edges_removed, 0u);
  EXPECT_EQ(store->getInEdges().size(), 1u);
}

TEST(ClassicalAdaptersTest, SvfgPreparationPrunesNonRecursiveLocalStore) {
  const char *source = R"(
    define void @worker() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      ret void
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);
  const auto *worker = module->getFunction("worker");
  ASSERT_NE(worker, nullptr);

  const llvm::AllocaInst *allocation = nullptr;
  const llvm::StoreInst *store_instruction = nullptr;
  for (const llvm::Instruction &instruction : llvm::instructions(*worker)) {
    allocation = allocation ? allocation
                            : llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    store_instruction = store_instruction
                            ? store_instruction
                            : llvm::dyn_cast<llvm::StoreInst>(&instruction);
  }
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(store_instruction, nullptr);

  SVFG svfg;
  svfg.initializeRefinedCallGraph(*module);
  auto *previous = new CopySVFGNode(1, nullptr, nullptr);
  auto *store = new StoreSVFGNode(2, nullptr, store_instruction, 3);
  store->setMemoryDef(7, 1, {42});
  svfg.addNode(previous);
  svfg.addNode(store);
  svfg.addEdge(previous, store, SVFGEdgeK::IntraIndirect, nullptr, {42});
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.isSingleton = true;
  svfg.setObjectInfo(42, info);
  svfg.setObjectValue(42, allocation);

  const SVFGPreparationStatistics stats = prepareSVFGForCFL(svfg);
  EXPECT_EQ(stats.strong_update_stores, 1u);
  EXPECT_EQ(stats.strong_update_edges_removed, 1u);
  EXPECT_TRUE(store->getInEdges().empty());
}

TEST(ClassicalAdaptersTest, SvfgPreparationPreservesRecursiveLocalStore) {
  const char *source = R"(
    define void @recursive() {
    entry:
      %slot = alloca i8*
      store i8* null, i8** %slot
      call void @recursive()
      ret void
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);
  const auto *recursive = module->getFunction("recursive");
  ASSERT_NE(recursive, nullptr);

  const llvm::AllocaInst *allocation = nullptr;
  const llvm::StoreInst *store_instruction = nullptr;
  for (const llvm::Instruction &instruction : llvm::instructions(*recursive)) {
    allocation = allocation ? allocation
                            : llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    store_instruction = store_instruction
                            ? store_instruction
                            : llvm::dyn_cast<llvm::StoreInst>(&instruction);
  }
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(store_instruction, nullptr);

  SVFG svfg;
  svfg.initializeRefinedCallGraph(*module);
  auto *previous = new CopySVFGNode(1, nullptr, nullptr);
  auto *store = new StoreSVFGNode(2, nullptr, store_instruction, 3);
  store->setMemoryDef(7, 1, {42});
  svfg.addNode(previous);
  svfg.addNode(store);
  svfg.addEdge(previous, store, SVFGEdgeK::IntraIndirect, nullptr, {42});
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.isSingleton = true;
  svfg.setObjectInfo(42, info);
  svfg.setObjectValue(42, allocation);

  const SVFGPreparationStatistics stats = prepareSVFGForCFL(svfg);
  EXPECT_EQ(stats.strong_update_stores, 0u);
  EXPECT_EQ(stats.strong_update_edges_removed, 0u);
  EXPECT_EQ(store->getInEdges().size(), 1u);
}

TEST(ClassicalAdaptersTest, SvfgPreparationPreservesLoopAllocationStore) {
  const char *source = R"(
    define void @worker(i1 %again) {
    entry:
      br label %loop
    loop:
      %slot = alloca i8*
      store i8* null, i8** %slot
      br i1 %again, label %loop, label %exit
    exit:
      ret void
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);
  const auto *worker = module->getFunction("worker");
  ASSERT_NE(worker, nullptr);

  const llvm::AllocaInst *allocation = nullptr;
  const llvm::StoreInst *store_instruction = nullptr;
  for (const llvm::Instruction &instruction : llvm::instructions(*worker)) {
    if (!allocation) {
      allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    }
    if (!store_instruction) {
      store_instruction = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    }
  }
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(store_instruction, nullptr);

  SVFG svfg;
  svfg.initializeRefinedCallGraph(*module);
  auto *previous = new CopySVFGNode(1, nullptr, nullptr);
  auto *store = new StoreSVFGNode(2, nullptr, store_instruction, 3);
  store->setMemoryDef(7, 1, {42});
  svfg.addNode(previous);
  svfg.addNode(store);
  svfg.addEdge(previous, store, SVFGEdgeK::IntraIndirect, nullptr, {42});
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.isSingleton = false;
  svfg.setObjectInfo(42, info);
  svfg.setObjectValue(42, allocation);

  const SVFGPreparationStatistics stats = prepareSVFGForCFL(svfg);
  EXPECT_EQ(stats.strong_update_stores, 0u);
  EXPECT_EQ(stats.strong_update_edges_removed, 0u);
  EXPECT_EQ(store->getInEdges().size(), 1u);
}

TEST(ClassicalAdaptersTest, SvfgPreparationPreservesPartialStore) {
  const char *source = R"(
    define void @worker() {
    entry:
      %whole = alloca { i8*, i8* }
      %byte = bitcast { i8*, i8* }* %whole to i8*
      store i8 0, i8* %byte
      ret void
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);
  const auto *worker = module->getFunction("worker");
  ASSERT_NE(worker, nullptr);

  const llvm::AllocaInst *allocation = nullptr;
  const llvm::StoreInst *store_instruction = nullptr;
  for (const llvm::Instruction &instruction : llvm::instructions(*worker)) {
    if (!allocation) {
      allocation = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
    }
    if (!store_instruction) {
      store_instruction = llvm::dyn_cast<llvm::StoreInst>(&instruction);
    }
  }
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(store_instruction, nullptr);

  SVFG svfg;
  auto *previous = new CopySVFGNode(1, nullptr, nullptr);
  auto *store = new StoreSVFGNode(2, nullptr, store_instruction, 3);
  store->setMemoryDef(7, 1, {42});
  svfg.addNode(previous);
  svfg.addNode(store);
  svfg.addEdge(previous, store, SVFGEdgeK::IntraIndirect, nullptr, {42});
  SVFG::ObjectInfo info;
  info.isStack = true;
  info.isSingleton = true;
  svfg.setObjectInfo(42, info);
  svfg.setObjectValue(42, allocation);

  const SVFGPreparationStatistics stats = prepareSVFGForCFL(svfg);
  EXPECT_EQ(stats.strong_update_stores, 0u);
  EXPECT_EQ(stats.strong_update_edges_removed, 0u);
  EXPECT_EQ(store->getInEdges().size(), 1u);
}

TEST(ClassicalAdaptersTest, LlvmAliasAnalysisTreatsExtractValueAsUnknown) {
  const char *source = R"(
    define i32 @main(i1 %condition) {
    entry:
      %p = alloca i8
      %other = alloca i8
      %slot = alloca i8*
      %aggregate = insertvalue { i8* } undef, i8* %p, 0
      %q = extractvalue { i8* } %aggregate, 0
      %r = select i1 %condition, i8* %q, i8* %p
      store i8* %q, i8** %slot
      %s = load i8*, i8** %slot
      ret i32 0
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  LLVMCFLAliasAnalysis analysis;
  analysis.analyze(*module);
  const llvm::Value *p = nullptr;
  const llvm::Value *other = nullptr;
  const llvm::Value *q = nullptr;
  const llvm::Value *r = nullptr;
  const llvm::Value *s = nullptr;
  for (const llvm::Instruction &instruction :
       llvm::instructions(*module->getFunction("main"))) {
    if (instruction.getName() == "p") {
      p = &instruction;
    } else if (instruction.getName() == "other") {
      other = &instruction;
    } else if (instruction.getName() == "q") {
      q = &instruction;
    } else if (instruction.getName() == "r") {
      r = &instruction;
    } else if (instruction.getName() == "s") {
      s = &instruction;
    }
  }
  ASSERT_NE(p, nullptr);
  ASSERT_NE(other, nullptr);
  ASSERT_NE(q, nullptr);
  ASSERT_NE(r, nullptr);
  ASSERT_NE(s, nullptr);
  ASSERT_TRUE(analysis.nodeForValue(q).has_value());
  EXPECT_TRUE(analysis.mayAlias(p, q));
  EXPECT_TRUE(analysis.mayAlias(r, other));
  EXPECT_TRUE(analysis.mayAlias(s, other));
  const auto pointees = analysis.pointsTo(q);
  EXPECT_NE(std::find(pointees.begin(), pointees.end(), p), pointees.end());
}

TEST(ClassicalAdaptersTest, LlvmAliasAnalysisModelsMemmovePointerCopies) {
  const char *source = R"(
    declare void @llvm.memmove.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @main() {
    entry:
      %object = alloca i8
      %source = alloca i8*
      %destination = alloca i8*
      store i8* %object, i8** %source
      %source.bytes = bitcast i8** %source to i8*
      %destination.bytes = bitcast i8** %destination to i8*
      call void @llvm.memmove.p0i8.p0i8.i64(
          i8* %destination.bytes, i8* %source.bytes, i64 8, i1 false)
      %loaded = load i8*, i8** %destination
      ret i32 0
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  LLVMCFLAliasAnalysis analysis;
  analysis.analyze(*module);
  const llvm::Value *object = nullptr;
  const llvm::Value *loaded = nullptr;
  for (const llvm::Instruction &instruction :
       llvm::instructions(*module->getFunction("main"))) {
    if (instruction.getName() == "object") {
      object = &instruction;
    } else if (instruction.getName() == "loaded") {
      loaded = &instruction;
    }
  }
  ASSERT_NE(object, nullptr);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(analysis.mayAlias(object, loaded));
}

TEST(ClassicalAdaptersTest, LlvmAliasAnalysisModelsMemcopyRangesByField) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define i32 @main() {
    entry:
      %first.object = alloca i8
      %second.object = alloca i8
      %source = alloca { i8*, i8* }
      %partial = alloca { i8*, i8* }
      %full = alloca { i8*, i8* }
      %source.first = getelementptr { i8*, i8* },
          { i8*, i8* }* %source, i64 0, i32 0
      %source.second = getelementptr { i8*, i8* },
          { i8*, i8* }* %source, i64 0, i32 1
      store i8* %first.object, i8** %source.first
      store i8* %second.object, i8** %source.second
      %source.bytes = bitcast { i8*, i8* }* %source to i8*
      %partial.bytes = bitcast { i8*, i8* }* %partial to i8*
      %full.bytes = bitcast { i8*, i8* }* %full to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(
          i8* %partial.bytes, i8* %source.bytes, i64 8, i1 false)
      call void @llvm.memcpy.p0i8.p0i8.i64(
          i8* %full.bytes, i8* %source.bytes, i64 16, i1 false)
      %partial.first = getelementptr { i8*, i8* },
          { i8*, i8* }* %partial, i64 0, i32 0
      %partial.second = getelementptr { i8*, i8* },
          { i8*, i8* }* %partial, i64 0, i32 1
      %full.second = getelementptr { i8*, i8* },
          { i8*, i8* }* %full, i64 0, i32 1
      %partial.first.loaded = load i8*, i8** %partial.first
      %partial.second.loaded = load i8*, i8** %partial.second
      %full.second.loaded = load i8*, i8** %full.second
      ret i32 0
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  LLVMCFLAliasAnalysis analysis;
  analysis.analyze(*module);
  const llvm::Value *first_object = nullptr;
  const llvm::Value *second_object = nullptr;
  const llvm::Value *partial_first = nullptr;
  const llvm::Value *partial_second = nullptr;
  const llvm::Value *full_second = nullptr;
  for (const llvm::Instruction &instruction :
       llvm::instructions(*module->getFunction("main"))) {
    if (instruction.getName() == "first.object") {
      first_object = &instruction;
    } else if (instruction.getName() == "second.object") {
      second_object = &instruction;
    } else if (instruction.getName() == "partial.first.loaded") {
      partial_first = &instruction;
    } else if (instruction.getName() == "partial.second.loaded") {
      partial_second = &instruction;
    } else if (instruction.getName() == "full.second.loaded") {
      full_second = &instruction;
    }
  }
  ASSERT_NE(first_object, nullptr);
  ASSERT_NE(second_object, nullptr);
  ASSERT_NE(partial_first, nullptr);
  ASSERT_NE(partial_second, nullptr);
  ASSERT_NE(full_second, nullptr);
  EXPECT_TRUE(analysis.mayAlias(first_object, partial_first));
  EXPECT_FALSE(analysis.mayAlias(second_object, partial_second));
  EXPECT_TRUE(analysis.mayAlias(second_object, full_second));
}

TEST(ClassicalAdaptersTest, LlvmAliasAnalysisProjectsFieldObjectsToAllocation) {
  const char *source = R"(
    define i32 @main() {
    entry:
      %object = alloca [16 x i8]
      %field = getelementptr [16 x i8], [16 x i8]* %object, i64 0, i64 8
      ret i32 0
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  LLVMCFLAliasAnalysis analysis;
  analysis.analyze(*module);
  const llvm::Value *object = nullptr;
  const llvm::Value *field = nullptr;
  for (const llvm::Instruction &instruction :
       llvm::instructions(*module->getFunction("main"))) {
    if (instruction.getName() == "object") {
      object = &instruction;
    } else if (instruction.getName() == "field") {
      field = &instruction;
    }
  }
  ASSERT_NE(object, nullptr);
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(analysis.pointsTo(field),
            std::vector<const llvm::Value *>({object}));
}

TEST(ClassicalAdaptersTest,
     LlvmAliasAnalysisRevisitsMemcopyInDiscoveredCallee) {
  const char *source = R"(
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

    define void @copy(i8** %destination, i8** %source) {
    entry:
      %destination.bytes = bitcast i8** %destination to i8*
      %source.bytes = bitcast i8** %source to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(
          i8* %destination.bytes, i8* %source.bytes, i64 8, i1 false)
      ret void
    }

    define i32 @main() {
    entry:
      %object = alloca i8
      %source = alloca i8*
      %destination = alloca i8*
      %function.slot = alloca void (i8**, i8**)*
      store i8* %object, i8** %source
      store void (i8**, i8**)* @copy,
            void (i8**, i8**)** %function.slot
      %target = load void (i8**, i8**)*,
                     void (i8**, i8**)** %function.slot
      call void %target(i8** %destination, i8** %source)
      %loaded = load i8*, i8** %destination
      ret i32 0
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  LLVMCFLAliasAnalysis analysis;
  const ReachabilityStats stats = analysis.analyze(*module);
  const llvm::Value *object = nullptr;
  const llvm::Value *loaded = nullptr;
  for (const llvm::Instruction &instruction :
       llvm::instructions(*module->getFunction("main"))) {
    if (instruction.getName() == "object") {
      object = &instruction;
    } else if (instruction.getName() == "loaded") {
      loaded = &instruction;
    }
  }
  ASSERT_NE(object, nullptr);
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(analysis.mayAlias(object, loaded));
  EXPECT_GE(stats.solver_rounds, 2u);
}

TEST(ClassicalAdaptersTest, LlvmAliasAnalysisDrivesIndirectCallDiscovery) {
  const char *source = R"(
    define void @callee(i8* %p, i8* %q) {
    entry:
      ret void
    }

    define i32 @main() {
    entry:
      %x = alloca i8
      %y = alloca i8
      %fp = alloca void (i8*, i8*)*
      store void (i8*, i8*)* @callee, void (i8*, i8*)** %fp
      %target = load void (i8*, i8*)*, void (i8*, i8*)** %fp
      call void %target(i8* %x, i8* %y)
      ret i32 0
    }
  )";
  llvm::LLVMContext context;
  auto module = parseModule(context, source);
  ASSERT_NE(module, nullptr);

  LLVMAliasOptions options;
  options.backend = SolverBackend::SparseBitVector;
  LLVMCFLAliasAnalysis analysis(options);
  const ReachabilityStats stats = analysis.analyze(*module);

  const llvm::Value *x = nullptr;
  const llvm::Value *y = nullptr;
  const llvm::Value *target = nullptr;
  for (const llvm::Instruction &instruction :
       llvm::instructions(*module->getFunction("main"))) {
    if (instruction.getName() == "x") {
      x = &instruction;
    } else if (instruction.getName() == "y") {
      y = &instruction;
    } else if (instruction.getName() == "target") {
      target = &instruction;
    }
  }
  ASSERT_NE(x, nullptr);
  ASSERT_NE(y, nullptr);
  ASSERT_NE(target, nullptr);
  EXPECT_TRUE(analysis.nodeForValue(target).has_value());
  EXPECT_TRUE(analysis.mayAlias(x, x));
  EXPECT_FALSE(analysis.mayAlias(x, y));
  llvm::Argument unmapped(llvm::Type::getInt8PtrTy(module->getContext()),
                          "unmapped");
  EXPECT_FALSE(analysis.nodeForValue(&unmapped).has_value());
  EXPECT_TRUE(analysis.mayAlias(x, &unmapped));
  EXPECT_GE(stats.solver_rounds, 2u);

  for (AliasEncodingMode encoding :
       {AliasEncodingMode::PAG, AliasEncodingMode::PEG}) {
    for (SolverBackend backend :
         {SolverBackend::SparseSet, SolverBackend::SparseBitVector,
          SolverBackend::Graspan, SolverBackend::Sqid, SolverBackend::Pearl,
          SolverBackend::TransitiveClosure, SolverBackend::Pocr,
          SolverBackend::HierarchicalPocr, SolverBackend::FullyOrdered,
          SolverBackend::EndpointQuotient}) {
      LLVMAliasOptions alternate_options;
      alternate_options.encoding = encoding;
      alternate_options.backend = backend;
      LLVMCFLAliasAnalysis alternate(alternate_options);
      EXPECT_NO_THROW(alternate.analyze(*module)) << solverBackendName(backend);
      EXPECT_FALSE(alternate.mayAlias(x, y)) << solverBackendName(backend);
      EXPECT_TRUE(alternate.nodeForValue(target).has_value())
          << solverBackendName(backend);
    }
  }

  for (AliasEncodingMode encoding :
       {AliasEncodingMode::PAG, AliasEncodingMode::PEG}) {
    for (engines::SpecializedPocrBackend backend :
         {engines::SpecializedPocrBackend::Pocr,
          engines::SpecializedPocrBackend::Focr}) {
      LLVMAliasOptions specialized_options;
      specialized_options.encoding = encoding;
      specialized_options.specialized_backend = backend;
      LLVMCFLAliasAnalysis specialized(specialized_options);
      EXPECT_NO_THROW(specialized.analyze(*module));
      EXPECT_FALSE(specialized.mayAlias(x, y));
      EXPECT_TRUE(specialized.nodeForValue(target).has_value());
    }
  }
}

} // namespace
