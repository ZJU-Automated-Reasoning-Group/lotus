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
  graphConfig.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool pointsToY = false;
  std::string observed;
  raw_string_ostream observedStream(observed);
  for (const auto &entry : *graph) {
    const auto *store = dyn_cast<StoreSVFGNode>(entry.second);
    if (!store)
      continue;
    observedStream << "store " << store->getId() << " targets=";
    if (const auto *storeInst =
            dyn_cast_or_null<StoreInst>(store->getInstruction())) {
      const Value *storedValue = storeInst->getValueOperand();
      const SVFGNode *storedNode = graph->getValueNode(storedValue);
      observedStream << " valueNode="
                     << (storedNode ? storedNode->getId() : 9999)
                     << " valuePts={";
      if (storedNode)
        for (uint32_t pointee : solver.pointsTo(storedNode))
          observedStream << pointee << ",";
      observedStream << "}; ";
    }
    for (uint32_t targetObject : store->getMemoryPointsTo()) {
      const SVFG::ObjectInfo *targetInfo = graph->getObjectInfo(targetObject);
      const Value *targetValue = graph->getObjectValue(targetObject);
      observedStream << " targetValue=";
      if (targetValue)
        observedStream << *targetValue;
      else
        observedStream << "<unknown>";
      observedStream << " constant=" << (targetInfo && targetInfo->isConstant)
                     << "; ";
      observedStream << targetObject << " out={";
      for (uint32_t pointee : solver.memoryOut(store, targetObject))
        observedStream << pointee << ",";
      observedStream << "}; ";
    }
  }
  observedStream << "load targets=";
  const auto *loadNode = dyn_cast<LoadSVFGNode>(graph->getDef(load));
  if (loadNode)
    for (uint32_t targetObject : loadNode->getMemoryPointsTo()) {
      observedStream << targetObject << " in={";
      for (uint32_t pointee : solver.memoryIn(loadNode, targetObject))
        observedStream << pointee << ",";
      observedStream << "}; ";
    }
  observedStream << "result=";
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    observedStream << object << "=";
    if (value)
      observedStream << *value;
    else
      observedStream << "<unknown>";
    observedStream << "; ";
    pointsToY |=
        value && value->stripPointerCasts() == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(pointsToY) << observedStream.str();
}

TEST_F(FlowSensitivePTATest, HashConsedBackendKeepsGlobalInitializerSets) {
  auto module = parseModule(R"(
    @x = global i8 0
    @slot = global i8* @x
    define i8* @main() {
    entry:
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
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const auto *load = dyn_cast<LoadInst>(
      &*module->getFunction("main")->getEntryBlock().begin());
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA::Config config;
  config.setBackend = PointsToSetBackend::HashConsed;
  FlowSensitivePTA solver(*graph, config);
  solver.solve();

  auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    const Value *value = graph->getObjectValue(object);
    return value &&
           value->stripPointerCasts() == module->getGlobalVariable("x");
  }));
}

TEST_F(FlowSensitivePTATest, RestartsAfterOnTheFlyIndirectCallConnection) {
  auto module = parseModule(R"(
    define i8* @target(i8* %arg) { ret i8* %arg }
    define i8* @main(i8* %arg, i8* (i8*)* %fp) {
    entry:
      %result = call i8* %fp(i8* %arg)
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);
  const Function *target = module->getFunction("target");
  const CallBase *call = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<CallBase>(&instruction))
      call = candidate;
  ASSERT_NE(target, nullptr);
  ASSERT_NE(call, nullptr);

  constexpr uint32_t functionObject = 300;
  constexpr uint32_t valueID = 400;
  SVFG graph;
  auto *function = new AddrSVFGNode(1, nullptr, target, functionObject);
  function->setValueId(valueID);
  graph.addNode(function);
  graph.setObjectValue(functionObject, target);
  SVFG::ObjectInfo info;
  info.isFunction = true;
  info.isSingleton = true;
  graph.setObjectInfo(functionObject, info);
  graph.addIndCallSite(valueID, call);

  std::size_t connections = 0;
  FlowSensitivePTA::Config config;
  config.connectIndirectCall = [&](const CallBase *site,
                                   const Function *callee) {
    EXPECT_EQ(site, call);
    EXPECT_EQ(callee, target);
    ++connections;
    return connections == 1;
  };
  FlowSensitivePTA solver(graph, config);
  const auto &stats = solver.solve();
  EXPECT_EQ(connections, 1u);
  EXPECT_EQ(stats.indirectCallEdges, 1u);
}

TEST_F(FlowSensitivePTATest, IndirectGuardPropagatesFieldInsensitiveFields) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t baseObject = 20;
  constexpr uint32_t fieldObject = 21;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {fieldObject});
  auto *load = new LoadSVFGNode(3, nullptr, nullptr, 0);
  load->setMemoryUse(1, 1, {fieldObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, store, load})
    graph.addNode(node);
  SVFG::ObjectInfo base;
  base.isFieldInsensitive = true;
  base.baseObjId = baseObject;
  graph.setObjectInfo(baseObject, base);
  SVFG::ObjectInfo field;
  field.baseObjId = baseObject;
  graph.setObjectInfo(fieldObject, field);
  graph.addEdge(x, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(store, load, SVFGEdgeK::IntraIndirect, nullptr, {baseObject});

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_EQ(solver.memoryIn(load, fieldObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.pointsTo(load), PointsToSet({xObject}));
}

TEST_F(FlowSensitivePTATest, HeapTargetsRemainWeakUpdates) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t yObject = 11;
  constexpr uint32_t heapObject = 20;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *first = new StoreSVFGNode(3, nullptr, nullptr, 0);
  first->setMemoryDef(1, 1, {heapObject});
  auto *second = new StoreSVFGNode(4, nullptr, nullptr, 0);
  second->setMemoryDef(1, 2, {heapObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, first, second})
    graph.addNode(node);
  SVFG::ObjectInfo heap;
  heap.isHeap = true;
  graph.setObjectInfo(heapObject, heap);
  graph.addEdge(x, first, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, second, SVFGEdgeK::IntraDirect);
  graph.addEdge(first, second, SVFGEdgeK::IntraIndirect, nullptr, {heapObject});

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_EQ(solver.memoryOut(second, heapObject),
            PointsToSet({xObject, yObject}));
  EXPECT_GT(solver.statistics().weakUpdates, 0u);
}
