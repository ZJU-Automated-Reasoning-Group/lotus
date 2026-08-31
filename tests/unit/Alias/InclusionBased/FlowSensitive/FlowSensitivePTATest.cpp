#include "Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"

#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::alias;
using namespace lotus::unittest;
using PointsToSet = FlowSensitivePTA::PointsToSet;

class FlowSensitivePTATest : public LlvmModuleTest {};

TEST_F(FlowSensitivePTATest, MaintainsPerNodeMemoryInOutAndStrongUpdates) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    @y = global i8 0
    define i8* @f() {
    entry:
      store i8* @x, i8** @slot
      store i8* @y, i8** @slot
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);
  auto it = module->getFunction("f")->getEntryBlock().begin();
  auto *storeXInst = cast<StoreInst>(&*it++);
  auto *storeYInst = cast<StoreInst>(&*it++);
  auto *loadInst = cast<LoadInst>(&*it);

  constexpr uint32_t xObject = 100;
  constexpr uint32_t yObject = 101;
  constexpr uint32_t slotObject = 200;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *storeX = new StoreSVFGNode(3, nullptr, storeXInst, 0);
  storeX->setMemoryDef(1, 1, {slotObject});
  auto *storeY = new StoreSVFGNode(4, nullptr, storeYInst, 0);
  storeY->setMemoryDef(1, 2, {slotObject});
  auto *load = new LoadSVFGNode(5, nullptr, loadInst, 0);
  load->setMemoryUse(1, 2, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, storeX, storeY, load})
    graph.addNode(node);
  graph.setValueNode(module->getGlobalVariable("x"), x->getId());
  graph.setValueNode(module->getGlobalVariable("y"), y->getId());
  graph.setValueNode(loadInst, load->getId());
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(xObject, singleton);
  graph.setObjectInfo(yObject, singleton);
  graph.setObjectInfo(slotObject, singleton);
  graph.addEdge(x, storeX, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, storeY, SVFGEdgeK::IntraDirect);
  graph.addEdge(storeX, storeY, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});
  graph.addEdge(storeY, load, SVFGEdgeK::IntraIndirect, nullptr, {slotObject});

  FlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(solver.memoryOut(storeX, slotObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.memoryIn(storeY, slotObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.memoryOut(storeY, slotObject), PointsToSet({yObject}));
  EXPECT_EQ(solver.memoryIn(load, slotObject), PointsToSet({yObject}));
  EXPECT_EQ(solver.pointsTo(load), PointsToSet({yObject}));
  EXPECT_GT(stats.sccs, 0u);
  EXPECT_GT(stats.strongUpdates, 0u);

  FlowSensitivePTA::Config hashConfig;
  hashConfig.setBackend = PointsToSetBackend::HashConsed;
  FlowSensitivePTA hashSolver(graph, hashConfig);
  hashSolver.solve();
  EXPECT_EQ(hashSolver.pointsTo(load), solver.pointsTo(load));
  EXPECT_GT(hashSolver.statistics().hashConsedUniqueSets, 0u);
}

TEST_F(FlowSensitivePTATest, ThreadInterferenceProducesWeakMemoryJoin) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t yObject = 11;
  constexpr uint32_t slotObject = 20;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *first = new StoreSVFGNode(3, nullptr, nullptr, 0);
  first->setMemoryDef(1, 1, {slotObject});
  auto *second = new StoreSVFGNode(4, nullptr, nullptr, 0);
  second->setMemoryDef(1, 1, {slotObject});
  auto *load = new LoadSVFGNode(5, nullptr, nullptr, 0);
  load->setMemoryUse(1, 1, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, first, second, load})
    graph.addNode(node);
  graph.addEdge(x, first, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, second, SVFGEdgeK::IntraDirect);
  graph.addEdge(first, load, SVFGEdgeK::ThreadMHPIndirectVF, nullptr,
                {slotObject});
  graph.addEdge(second, load, SVFGEdgeK::ThreadMHPIndirectVF, nullptr,
                {slotObject});

  FlowSensitivePTA solver(graph);
  solver.solve();
  const PointsToSet expected{xObject, yObject};
  EXPECT_EQ(solver.memoryIn(load, slotObject), expected);
  EXPECT_EQ(solver.pointsTo(load), expected);
}

TEST_F(FlowSensitivePTATest, SolvesBuilderProducedSVFG) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      store i8* @x, i8** @slot
      store i8* @y, i8** @slot
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.memoryPartition =
      MemoryRegionPartitionStrategy::InterDisjoint;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction : instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  const uint32_t yObject = graph->getObjectId(module->getGlobalVariable("y"));
  ASSERT_NE(yObject, 0u);
  EXPECT_EQ(result->count(yObject), 1u);
}
