#include "CFL/Classical/SVFPort.h"
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGNode.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace lotus::analysis;
using namespace lotus::cfl::classical;
using namespace lotus::unittest;

namespace {

TEST(ClassicalSVFPortTest,
     PagAliasClientEncodesBidirectionalEdgesAndAnswersQueries) {
  AliasConstraintGraph graph;
  const auto obj = graph.addNode("obj");
  const auto ptr = graph.addNode("ptr");
  const auto alias = graph.addNode("alias");

  graph.addEdge(obj, ptr, AliasConstraintEdgeKind::Addr);
  graph.addEdge(ptr, alias, AliasConstraintEdgeKind::Copy);

  AliasClient client = AliasClient::fromConstraintGraph(graph);
  const auto stats = client.solve();

  EXPECT_TRUE(client.graph().hasEdge(obj, ptr, "addr"));
  EXPECT_TRUE(client.graph().hasEdge(ptr, obj, "addrbar"));
  EXPECT_TRUE(client.mayAlias(ptr, alias));

  const auto pts = client.pointsTo(alias);
  EXPECT_NE(std::find(pts.begin(), pts.end(), obj), pts.end());
  EXPECT_GT(stats.added_edges, 0u);
}

TEST(ClassicalSVFPortTest,
     PegEncodingRewritesLoadsAndStoresThroughSyntheticDeref) {
  AliasConstraintGraph graph;
  const auto obj = graph.addNode("obj");
  const auto slot = graph.addNode("slot");
  const auto value = graph.addNode("value");
  const auto loaded = graph.addNode("loaded");

  graph.addEdge(obj, slot, AliasConstraintEdgeKind::Addr);
  graph.addEdge(value, slot, AliasConstraintEdgeKind::Store);
  graph.addEdge(slot, loaded, AliasConstraintEdgeKind::Load);

  AliasClient client =
      AliasClient::fromConstraintGraph(graph, AliasEncodingMode::PEG);
  client.solve();

  EXPECT_TRUE(client.mayAlias(value, loaded));
  const auto pts = client.pointsTo(loaded);
  EXPECT_FALSE(pts.empty());
  EXPECT_EQ(pts.front(), obj);
}

TEST(ClassicalSVFPortTest, GrammarBuildersMaterializeObservedAttributes) {
  AliasConstraintGraph graph;
  const auto base = graph.addNode("base");
  const auto field = graph.addNode("field");
  graph.addEdge(base, field, AliasConstraintEdgeKind::NormalGep, 3);

  const auto pag = buildPagGrammar(graph);
  const auto peg = buildPegGrammar(graph);

  EXPECT_NE(pag.binaryByFirst().find("gepbar_3"), pag.binaryByFirst().end());
  EXPECT_NE(peg.binaryByFirst().find("gepbar_3"), peg.binaryByFirst().end());
}

TEST(ClassicalSVFPortTest, ValueFlowClientEncodesSvfgCallsAndReachability) {
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

  actual->setValueId(10);
  formal->setValueId(11);
  formal_ret->setValueId(12);
  actual_ret->setValueId(13);
  copy->setValueId(14);

  svfg.addNode(actual);
  svfg.addActualParm(callsite, actual);
  svfg.addNode(formal);
  svfg.addFormalParm(callee, formal);
  svfg.addNode(formal_ret);
  svfg.addFormalRet(callee, formal_ret);
  svfg.addNode(actual_ret);
  svfg.addActualRet(callsite, actual_ret);
  svfg.addNode(copy);
  svfg.addEdge(actual, formal, SVFGEdgeK::CallInd, callsite);
  svfg.addEdge(formal, copy, SVFGEdgeK::IntraCopy);
  svfg.addEdge(copy, formal_ret, SVFGEdgeK::IntraIndirect, nullptr, {42});
  svfg.addEdge(formal_ret, actual_ret, SVFGEdgeK::RetInd, callsite);

  ValueFlowClient client = ValueFlowClient::fromSVFG(svfg);
  const auto stats = client.solve();

  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("1"),
                                     client.graph().vertexId("2"), "call_1"));
  EXPECT_TRUE(client.graph().hasEdge(client.graph().vertexId("4"),
                                     client.graph().vertexId("3"), "retbar_1"));
  EXPECT_TRUE(client.hasFlow(1, 4));

  const auto reachable = client.reachableFrom(1);
  EXPECT_NE(std::find(reachable.begin(), reachable.end(), 4), reachable.end());
  EXPECT_GT(stats.added_edges, 0u);
}

} // namespace
