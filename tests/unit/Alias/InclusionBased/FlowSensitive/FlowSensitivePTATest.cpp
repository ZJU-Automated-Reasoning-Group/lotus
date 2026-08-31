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
  EXPECT_EQ(stats.strongUpdates, 2u);
  EXPECT_EQ(stats.weakUpdates, 0u);
  EXPECT_GE(stats.strongUpdateExecutions, stats.strongUpdates);

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

TEST_F(FlowSensitivePTATest, AggregateGlobalInitializersRespectFieldOffsets) {
  auto module = parseModule(R"(
    %G = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    @g = global %G { i8* @x, i8* @y }
    define i8* @main() {
    entry:
      %result = load i8*, i8** getelementptr (%G, %G* @g, i32 0, i32 0)
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
  FlowSensitivePTA solver(*graph);
  solver.solve();

  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    hasX |= value == module->getGlobalVariable("x");
    hasY |= value == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(hasX);
  EXPECT_FALSE(hasY);
}

TEST_F(FlowSensitivePTATest, ConstantGepGlobalInitializerKeepsExactField) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    @aggregate = global %S { i8* @x, i8* @y }
    @field.pointer = global i8** getelementptr (%S, %S* @aggregate,
                                                i32 0, i32 1)
    define i8* @main() {
    entry:
      %field = load i8**, i8*** @field.pointer
      %result = load i8*, i8** %field
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

  const LoadInst *fieldLoad = nullptr;
  const LoadInst *resultLoad = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    const auto *load = dyn_cast<LoadInst>(&instruction);
    if (load && load->getName() == "field")
      fieldLoad = load;
    else if (load && load->getName() == "result")
      resultLoad = load;
  }
  ASSERT_NE(fieldLoad, nullptr);
  ASSERT_NE(resultLoad, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto fieldResult = solver.pointsTo(fieldLoad);
  ASSERT_TRUE(fieldResult.has_value());
  ASSERT_EQ(fieldResult->size(), 1u);
  const SVFG::ObjectInfo *fieldInfo =
      graph->getObjectInfo(*fieldResult->begin());
  ASSERT_NE(fieldInfo, nullptr);
  EXPECT_TRUE(fieldInfo->hasFieldOffset);
  EXPECT_EQ(fieldInfo->fieldOffset, 8u);

  const uint32_t aggregateObject =
      graph->getObjectId(module->getGlobalVariable("aggregate"));
  ASSERT_NE(aggregateObject, 0u);
  EXPECT_NE(*fieldResult->begin(), aggregateObject);
}

TEST_F(FlowSensitivePTATest, StrongUpdateDoesNotReintroduceGlobalInitializer) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @slot = global i8* @x
    define i8* @main() {
    entry:
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
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    hasX |= value == module->getGlobalVariable("x");
    hasY |= value == module->getGlobalVariable("y");
  }
  EXPECT_FALSE(hasX);
  EXPECT_TRUE(hasY);
}

TEST_F(FlowSensitivePTATest, EmptyIndirectGuardDoesNotPropagateMemory) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f() { ret void }
  )");
  ASSERT_NE(module, nullptr);
  constexpr uint32_t xObject = 10;
  constexpr uint32_t slotObject = 20;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {slotObject});
  auto *load = new LoadSVFGNode(3, nullptr, nullptr, 0);
  load->setMemoryUse(1, 1, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, store, load})
    graph.addNode(node);
  SVFG::ObjectInfo singleton;
  singleton.isSingleton = true;
  graph.setObjectInfo(slotObject, singleton);
  graph.addEdge(x, store, SVFGEdgeK::IntraDirect);
  graph.addEdge(store, load, SVFGEdgeK::IntraIndirect);

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_TRUE(solver.memoryIn(load, slotObject).empty());
  EXPECT_TRUE(solver.pointsTo(load).empty());
}

TEST_F(FlowSensitivePTATest, RefinedIndirectRecursionDisablesStrongUpdate) {
  auto module = parseModule(R"(
    @x = global i8 0
    define void @f(void ()* %fp) {
    entry:
      %slot = alloca i8*
      call void %fp()
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  Function *function = module->getFunction("f");
  ASSERT_NE(function, nullptr);
  const auto *allocation =
      dyn_cast<AllocaInst>(&*function->getEntryBlock().begin());
  const auto *call =
      dyn_cast<CallBase>(&*std::next(function->getEntryBlock().begin()));
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(call, nullptr);

  constexpr uint32_t xObject = 10;
  constexpr uint32_t slotObject = 20;
  SVFG graph;
  graph.initializeRefinedCallGraph(*module);
  ASSERT_TRUE(graph.markConnectedCallee(call, function));
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *store = new StoreSVFGNode(2, nullptr, nullptr, 0);
  store->setMemoryDef(1, 1, {slotObject});
  graph.addNode(x);
  graph.addNode(store);
  graph.addEdge(x, store, SVFGEdgeK::IntraDirect);
  graph.setObjectValue(slotObject, allocation);
  SVFG::ObjectInfo stack;
  stack.isStack = true;
  stack.isSingleton = true;
  graph.setObjectInfo(slotObject, stack);

  FlowSensitivePTA solver(graph);
  solver.solve();
  EXPECT_EQ(solver.memoryOut(store, slotObject), PointsToSet({xObject}));
  EXPECT_EQ(solver.statistics().strongUpdates, 0u);
  EXPECT_GT(solver.statistics().weakUpdates, 0u);
}

TEST_F(FlowSensitivePTATest, MemcpyNullFieldKillsDestinationFact) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* null, i8** %source.field
      store i8* @x, i8** %destination.field
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 8, i1 false)
      %result = load i8*, i8** %destination.field
      ret i8* %result
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
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

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(FlowSensitivePTATest, MemcpyUsesPointerSizeOfFieldAddressSpace) {
  auto module = parseModule(R"(
    target datalayout = "e-p:64:64-p1:32:32"
    %S = type { i8 addrspace(1)* }
    @x = addrspace(1) global i8 0
    define i8 addrspace(1)* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8 addrspace(1)* null, i8 addrspace(1)** %source.field
      store i8 addrspace(1)* @x, i8 addrspace(1)** %destination.field
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 4, i1 false)
      %result = load i8 addrspace(1)*, i8 addrspace(1)** %destination.field
      ret i8 addrspace(1)* %result
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
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

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(FlowSensitivePTATest, PartialMemcpyPreservesFieldsOutsideRange) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    @z = global i8 0
    @w = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.0 = getelementptr %S, %S* %source, i32 0, i32 0
      %source.1 = getelementptr %S, %S* %source, i32 0, i32 1
      %destination.0 = getelementptr %S, %S* %destination, i32 0, i32 0
      %destination.1 = getelementptr %S, %S* %destination, i32 0, i32 1
      store i8* @x, i8** %source.0
      store i8* @y, i8** %source.1
      store i8* @z, i8** %destination.0
      store i8* @w, i8** %destination.1
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 8, i1 false)
      %copied = load i8*, i8** %destination.0
      %preserved = load i8*, i8** %destination.1
      ret i8* %copied
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
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

  const LoadInst *copied = nullptr;
  const LoadInst *preserved = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    const auto *load = dyn_cast<LoadInst>(&instruction);
    if (!load)
      continue;
    if (load->getName() == "copied")
      copied = load;
    else if (load->getName() == "preserved")
      preserved = load;
  }
  ASSERT_NE(copied, nullptr);
  ASSERT_NE(preserved, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  auto pointsToGlobal = [&](const LoadInst *load, StringRef name) {
    const auto result = solver.pointsTo(load);
    if (!result)
      return false;
    return std::any_of(result->begin(), result->end(), [&](uint32_t object) {
      return graph->getObjectValue(object) == module->getGlobalVariable(name);
    });
  };
  EXPECT_TRUE(pointsToGlobal(copied, "x"));
  EXPECT_FALSE(pointsToGlobal(copied, "z"));
  EXPECT_TRUE(pointsToGlobal(preserved, "w"));
  EXPECT_FALSE(pointsToGlobal(preserved, "y"));
}

TEST_F(FlowSensitivePTATest, PartialPointerOverwriteBecomesUnknown) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* null, i8** %source.field
      store i8* @x, i8** %destination.field
      %source.bytes = bitcast %S* %source to i8*
      %destination.bytes = bitcast %S* %destination to i8*
      call void @llvm.memcpy.p0i8.p0i8.i64(i8* %destination.bytes,
                                           i8* %source.bytes,
                                           i64 4, i1 false)
      %result = load i8*, i8** %destination.field
      ret i8* %result
    }
    declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)
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

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);
  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->isUnknownObject(object);
  }));
  EXPECT_FALSE(
      std::any_of(result->begin(), result->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == module->getGlobalVariable("x");
      }));
}

TEST_F(FlowSensitivePTATest, NegativeGepIndexDoesNotCreateUnsignedOffset) {
  auto module = parseModule(R"(
    define i8** @main(i8** %pointer) {
    entry:
      %previous = getelementptr i8*, i8** %pointer, i64 -1
      ret i8** %previous
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

  const auto *gep = dyn_cast<GetElementPtrInst>(
      &*module->getFunction("main")->getEntryBlock().begin());
  ASSERT_NE(gep, nullptr);
  EXPECT_FALSE(graph->getGepAccess(gep).valid);
}

TEST_F(FlowSensitivePTATest, FieldInsensitiveBaseDoesNotCreateFieldObject) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    define i8** @main() {
    entry:
      %object = alloca %S
      %field = getelementptr %S, %S* %object, i32 0, i32 1
      ret i8** %field
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.memModelType = SVFGBuilderConfig::MemModelType::FieldInsensitive;
  SVFGBuilder graphBuilder(graphConfig);
  std::unique_ptr<SVFG> graph(graphBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const AllocaInst *allocation = nullptr;
  const GetElementPtrInst *gep = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    allocation = allocation ? allocation : dyn_cast<AllocaInst>(&instruction);
    gep = gep ? gep : dyn_cast<GetElementPtrInst>(&instruction);
  }
  ASSERT_NE(allocation, nullptr);
  ASSERT_NE(gep, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto base = solver.pointsTo(allocation);
  const auto field = solver.pointsTo(gep);
  ASSERT_TRUE(base.has_value());
  ASSERT_TRUE(field.has_value());
  EXPECT_EQ(*field, *base);
}

TEST_F(FlowSensitivePTATest, CanonicalGepOffsetsSeparateStructFields) {
  auto module = parseModule(R"(
    %S = type { i8*, i8* }
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      %object = alloca %S
      %field0 = getelementptr %S, %S* %object, i32 0, i32 0
      %field1 = getelementptr %S, %S* %object, i32 0, i32 1
      store i8* @x, i8** %field0
      store i8* @y, i8** %field1
      %result = load i8*, i8** %field0
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

  const GetElementPtrInst *field0 = nullptr;
  const GetElementPtrInst *field1 = nullptr;
  const LoadInst *resultLoad = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    if (const auto *gep = dyn_cast<GetElementPtrInst>(&instruction)) {
      if (gep->getName() == "field0")
        field0 = gep;
      else if (gep->getName() == "field1")
        field1 = gep;
    } else if (const auto *load = dyn_cast<LoadInst>(&instruction)) {
      resultLoad = load;
    }
  }
  ASSERT_NE(field0, nullptr);
  ASSERT_NE(field1, nullptr);
  ASSERT_NE(resultLoad, nullptr);
  EXPECT_EQ(graph->getValueNode(field0), graph->getDef(field0));
  EXPECT_EQ(graph->getValueNode(field1), graph->getDef(field1));

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(resultLoad);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->getObjectValue(object) == module->getGlobalVariable("x");
  }));
  EXPECT_FALSE(
      std::any_of(result->begin(), result->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == module->getGlobalVariable("y");
      }));
}

TEST_F(FlowSensitivePTATest, ArrayElementsUseWeakUpdates) {
  auto module = parseModule(R"(
    %A = type { [2 x i8*] }
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      %object = alloca %A
      %element0 = getelementptr %A, %A* %object, i32 0, i32 0, i64 0
      %element1 = getelementptr %A, %A* %object, i32 0, i32 0, i64 1
      store i8* @x, i8** %element0
      store i8* @y, i8** %element1
      %result = load i8*, i8** %element0
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

  const LoadInst *resultLoad = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *load = dyn_cast<LoadInst>(&instruction))
      resultLoad = load;
  ASSERT_NE(resultLoad, nullptr);

  FlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(resultLoad);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    const Value *value = graph->getObjectValue(object);
    hasX |= value == module->getGlobalVariable("x");
    hasY |= value == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(hasX);
  EXPECT_TRUE(hasY);
  EXPECT_GT(solver.statistics().weakUpdates, 0u);
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
