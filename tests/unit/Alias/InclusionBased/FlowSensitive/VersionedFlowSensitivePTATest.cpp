#include "Alias/InclusionBased/FlowSensitive/VersionedFlowSensitivePTA.h"

#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <unistd.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::alias;
using namespace lotus::unittest;
using PointsToSet = VersionedFlowSensitivePTA::PointsToSet;

class VersionedFlowSensitivePTATest : public LlvmModuleTest {};

TEST_F(VersionedFlowSensitivePTATest,
       ObjectsWithIdenticalFootprintsReuseVersionLabels) {
  constexpr uint32_t firstObject = 100;
  constexpr uint32_t secondObject = 101;
  SVFG graph;
  graph.addNode(new DummySVFGNode(1, nullptr));
  graph.setObjectDebug(firstObject, "first");
  graph.setObjectDebug(secondObject, "second");
  SVFG::ObjectInfo info;
  info.isGlobal = true;
  info.isSingleton = true;
  graph.setObjectInfo(firstObject, info);
  graph.setObjectInfo(secondObject, info);

  VersionedFlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(solver.canonicalVersionObject(firstObject), firstObject);
  EXPECT_EQ(solver.canonicalVersionObject(secondObject), firstObject);
  EXPECT_EQ(stats.equivalentObjects, 1u);
  EXPECT_EQ(solver.getConsume(1, firstObject),
            solver.getConsume(1, secondObject));
}

TEST_F(VersionedFlowSensitivePTATest,
       IndirectFunctionEntryUsesDedicatedDeltaVersion) {
  auto module = parseModule(R"(
    define void @target(i8** %pointer) {
    entry:
      ret void
    }
    define i32 @main(void (i8**)* %function.pointer, i8** %pointer) {
    entry:
      call void %function.pointer(i8** %pointer)
      ret i32 0
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

  constexpr uint32_t object = 100;
  SVFG graph;
  auto *root = new DummySVFGNode(1, nullptr);
  auto *formalIn =
      new FormalInSVFGNode(2, nullptr, target, 1, PointsToSet{object}, 1);
  graph.addNode(root);
  graph.addNode(formalIn);
  graph.setObjectDebug(object, "memory");
  graph.addCalleeToIndCallSite(target, call);

  VersionedFlowSensitivePTA solver(graph);
  solver.solve();
  const auto rootVersion = solver.getConsume(root->getId(), object);
  const auto deltaVersion = solver.getConsume(formalIn->getId(), object);
  EXPECT_NE(rootVersion, VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(deltaVersion, VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(rootVersion, deltaVersion);
  EXPECT_EQ(deltaVersion, solver.getYield(formalIn->getId(), object));
}

TEST_F(VersionedFlowSensitivePTATest,
       NewlyDiscoveredDeltaNodeForcesVersionRelabeling) {
  auto module = parseModule(R"(
    define void @target(i8** %pointer) {
    entry:
      ret void
    }
    define i32 @main(void (i8**)* %function.pointer, i8** %pointer) {
    entry:
      call void %function.pointer(i8** %pointer)
      ret i32 0
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

  constexpr uint32_t functionObject = 100;
  constexpr uint32_t memoryObject = 200;
  constexpr uint32_t functionPointerId = 42;
  SVFG graph;
  auto *address = new AddrSVFGNode(1, nullptr, target, functionObject);
  address->setValueId(functionPointerId);
  auto *source = new DummySVFGNode(2, nullptr);
  auto *formalIn =
      new FormalInSVFGNode(3, nullptr, target, 1, PointsToSet{memoryObject}, 1);
  graph.addNode(address);
  graph.addNode(source);
  graph.addNode(formalIn);
  graph.setObjectValue(functionObject, target);
  SVFG::ObjectInfo functionInfo;
  functionInfo.isFunction = true;
  functionInfo.isConstant = true;
  graph.setObjectInfo(functionObject, functionInfo);
  graph.setObjectDebug(functionObject, "target");
  graph.setObjectDebug(memoryObject, "memory");
  graph.addIndCallSite(functionPointerId, call);

  bool connected = false;
  VersionedFlowSensitivePTA::Config config;
  config.connectIndirectCall = [&](const CallBase *callSite,
                                   const Function *callee) {
    if (connected || callSite != call || callee != target)
      return false;
    connected = true;
    graph.addCalleeToIndCallSite(target, call);
    graph.addEdge(source, formalIn, SVFGEdgeK::IntraIndirect, call,
                  PointsToSet{memoryObject});
    return true;
  };

  VersionedFlowSensitivePTA solver(graph, std::move(config));
  const auto &stats = solver.solve();
  EXPECT_TRUE(connected);
  EXPECT_GT(stats.relabelings, 1u);
  EXPECT_NE(solver.getConsume(source->getId(), memoryObject),
            solver.getConsume(formalIn->getId(), memoryObject));
}

TEST_F(VersionedFlowSensitivePTATest,
       StorePrelabelsProduceDistinctConsumeAndYieldVersions) {
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

  auto instruction = module->getFunction("main")->getEntryBlock().begin();
  auto *storeXInstruction = cast<StoreInst>(&*instruction++);
  auto *storeYInstruction = cast<StoreInst>(&*instruction++);
  auto *loadInstruction = cast<LoadInst>(&*instruction);

  constexpr uint32_t xObject = 100;
  constexpr uint32_t yObject = 101;
  constexpr uint32_t slotObject = 200;
  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *storeX = new StoreSVFGNode(3, nullptr, storeXInstruction, 0);
  auto *storeY = new StoreSVFGNode(4, nullptr, storeYInstruction, 0);
  auto *load = new LoadSVFGNode(5, nullptr, loadInstruction, 0);
  storeX->setMemoryDef(1, 1, {slotObject});
  storeY->setMemoryDef(1, 2, {slotObject});
  load->setMemoryUse(1, 2, {slotObject});
  for (SVFGNode *node : std::vector<SVFGNode *>{x, y, storeX, storeY, load})
    graph.addNode(node);
  graph.setValueNode(module->getGlobalVariable("x"), x->getId());
  graph.setValueNode(module->getGlobalVariable("y"), y->getId());
  graph.setValueNode(loadInstruction, load->getId());
  SVFG::ObjectInfo singleton;
  singleton.isGlobal = true;
  singleton.isSingleton = true;
  graph.setObjectInfo(xObject, singleton);
  graph.setObjectInfo(yObject, singleton);
  graph.setObjectInfo(slotObject, singleton);
  graph.setObjectDebug(slotObject, "slot");
  graph.addEdge(x, storeX, SVFGEdgeK::IntraDirect);
  graph.addEdge(y, storeY, SVFGEdgeK::IntraDirect);
  graph.addEdge(storeX, storeY, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});
  graph.addEdge(storeY, load, SVFGEdgeK::IntraIndirect, nullptr, {slotObject});

  VersionedFlowSensitivePTA solver(graph);
  const auto &stats = solver.solve();

  const auto storeXConsume = solver.getConsume(storeX->getId(), slotObject);
  const auto storeXYield = solver.getYield(storeX->getId(), slotObject);
  const auto storeYConsume = solver.getConsume(storeY->getId(), slotObject);
  const auto storeYYield = solver.getYield(storeY->getId(), slotObject);
  EXPECT_NE(storeXConsume, VersionedFlowSensitivePTA::InvalidVersion);
  EXPECT_NE(storeXConsume, storeXYield);
  EXPECT_EQ(storeXYield, storeYConsume);
  EXPECT_NE(storeYConsume, storeYYield);
  EXPECT_EQ(storeYYield, solver.getConsume(load->getId(), slotObject));
  EXPECT_EQ(solver.versionedPointsTo(slotObject, storeYYield),
            PointsToSet({yObject}));
  EXPECT_EQ(solver.pointsTo(load), PointsToSet({yObject}));
  const auto &dependentStatements =
      solver.getDependentStatements(slotObject, storeXConsume);
  EXPECT_NE(std::find(dependentStatements.begin(), dependentStatements.end(),
                      storeX->getId()),
            dependentStatements.end());
  EXPECT_EQ(stats.strongUpdates, 2u);
  EXPECT_GT(stats.statementReliances, 0u);
  EXPECT_GT(stats.versions, 1u);
}

TEST_F(VersionedFlowSensitivePTATest,
       StrongUpdateDoesNotMeldKilledGlobalInitializer) {
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
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  const StoreInst *store = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
    if (const auto *candidate = dyn_cast<StoreInst>(&instruction))
      store = candidate;
  }
  ASSERT_NE(load, nullptr);
  ASSERT_NE(store, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    hasX |= graph->getObjectValue(object) == module->getGlobalVariable("x");
    hasY |= graph->getObjectValue(object) == module->getGlobalVariable("y");
  }
  EXPECT_FALSE(hasX);
  EXPECT_TRUE(hasY);

  const auto *storeNode = dyn_cast_or_null<StoreSVFGNode>(graph->getDef(store));
  ASSERT_NE(storeNode, nullptr);
  ASSERT_EQ(storeNode->getMemoryPointsTo().size(), 1u);
  const uint32_t slotObject = *storeNode->getMemoryPointsTo().begin();
  const auto consumed = solver.getConsume(storeNode->getId(), slotObject);
  const auto yielded = solver.getYield(storeNode->getId(), slotObject);
  const auto &reliant = solver.getReliantVersions(slotObject, consumed);
  EXPECT_EQ(std::find(reliant.begin(), reliant.end(), yielded), reliant.end());

  SmallString<256> path;
  int fileDescriptor = -1;
  ASSERT_FALSE(sys::fs::createTemporaryFile("lotus-vfspta", "txt",
                                            fileDescriptor, path));
  ::close(fileDescriptor);
  ASSERT_TRUE(solver.writeVersionedAnalysisResultToFile(path.str().str()));

  VersionedFlowSensitivePTA restored(*graph);
  ASSERT_TRUE(restored.readVersionedAnalysisResultFromFile(path.str().str()));
  sys::fs::remove(path);
  EXPECT_EQ(restored.pointsTo(load), *result);
  EXPECT_EQ(restored.statistics().versionedFacts,
            solver.statistics().versionedFacts);
}

TEST_F(VersionedFlowSensitivePTATest,
       NonRecursiveCalleeStackObjectUsesStrongUpdates) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define i8* @helper() {
    entry:
      %slot = alloca i8*
      store i8* @x, i8** %slot
      store i8* @y, i8** %slot
      %result = load i8*, i8** %slot
      ret i8* %result
    }
    define i8* @main() {
    entry:
      %result = call i8* @helper()
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("helper")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  const auto &stats = solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->getObjectValue(object) == module->getGlobalVariable("y");
  }));
  EXPECT_EQ(stats.strongUpdates, 2u);
  EXPECT_EQ(stats.weakUpdates, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       RecursiveCalleeStackObjectUsesWeakUpdates) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    define i8* @recursive(i1 %stop) {
    entry:
      %slot = alloca i8*
      store i8* @x, i8** %slot
      br i1 %stop, label %exit, label %recurse
    recurse:
      %ignored = call i8* @recursive(i1 true)
      store i8* @y, i8** %slot
      br label %exit
    exit:
      %result = load i8*, i8** %slot
      ret i8* %result
    }
    define i8* @main() {
    entry:
      %result = call i8* @recursive(i1 false)
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  const auto &stats = solver.solve();
  EXPECT_EQ(stats.strongUpdates, 0u);
  EXPECT_GE(stats.weakUpdates, 2u);
}

TEST_F(VersionedFlowSensitivePTATest,
       MeldJoinCombinesBypassAndStrongUpdateVersions) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @slot = global i8* @x
    define i8* @main(i1 %condition) {
    entry:
      br i1 %condition, label %update, label %join
    update:
      store i8* @y, i8** @slot
      br label %join
    join:
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    hasX |= graph->getObjectValue(object) == module->getGlobalVariable("x");
    hasY |= graph->getObjectValue(object) == module->getGlobalVariable("y");
  }
  EXPECT_TRUE(hasX);
  EXPECT_TRUE(hasY);
}

TEST_F(VersionedFlowSensitivePTATest,
       MeldJoinWithTwoDefinitionsDoesNotRestoreInitializer) {
  auto module = parseModule(R"(
    @x = global i8 0
    @y = global i8 0
    @z = global i8 0
    @slot = global i8* @x
    define i8* @main(i1 %condition) {
    entry:
      br i1 %condition, label %left, label %right
    left:
      store i8* @y, i8** @slot
      br label %join
    right:
      store i8* @z, i8** @slot
      br label %join
    join:
      %result = load i8*, i8** @slot
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  auto pointsToGlobal = [&](StringRef name) {
    return std::any_of(result->begin(), result->end(), [&](uint32_t object) {
      return graph->getObjectValue(object) == module->getGlobalVariable(name);
    });
  };
  EXPECT_FALSE(pointsToGlobal("x"));
  EXPECT_TRUE(pointsToGlobal("y"));
  EXPECT_TRUE(pointsToGlobal("z"));
}

TEST_F(VersionedFlowSensitivePTATest,
       IndirectCallTopologyChangeTriggersVersionRelabeling) {
  auto module = parseModule(R"(
    @y = global i8 0
    @function.pointer = global void (i8**)* @target
    define void @target(i8** %pointer) {
    entry:
      store i8* @y, i8** %pointer
      ret void
    }
    define i32 @main() {
    entry:
      %slot = alloca i8*
      %callee = load void (i8**)*, void (i8**)** @function.pointer
      call void %callee(i8** %slot)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const Function *mainFunction = module->getFunction("main");
  const Function *targetFunction = module->getFunction("target");
  ASSERT_NE(mainFunction, nullptr);
  ASSERT_NE(targetFunction, nullptr);
  const CallBase *indirectCall = nullptr;
  for (const Instruction &instruction : instructions(mainFunction)) {
    const auto *call = dyn_cast<CallBase>(&instruction);
    if (call && !call->getCalledFunction())
      indirectCall = call;
  }
  ASSERT_NE(indirectCall, nullptr);
  const SVFGNode *functionPointerNode =
      graph->getValueNode(indirectCall->getCalledOperand());
  ASSERT_NE(functionPointerNode, nullptr);
  const uint32_t functionPointerId = functionPointerNode->hasValueId()
                                         ? functionPointerNode->getValueId()
                                         : functionPointerNode->getId();
  EXPECT_FALSE(graph->getIndCallSites(functionPointerId).empty());
  EXPECT_TRUE(graph->getConnectedCallees(indirectCall).empty());

  VersionedFlowSensitivePTA::Config solverConfig;
  std::size_t connectorCalls = 0;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    ++connectorCalls;
    if (callSite != indirectCall || target != targetFunction)
      return false;
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();
  const auto callees = solver.pointsTo(indirectCall->getCalledOperand());
  ASSERT_TRUE(callees.has_value());
  EXPECT_TRUE(
      std::any_of(callees->begin(), callees->end(), [&](uint32_t object) {
        return graph->getObjectValue(object) == targetFunction;
      }));
  EXPECT_GT(stats.indirectCallEdges, 0u);
  EXPECT_EQ(stats.relabelings, 1u);
  EXPECT_GT(stats.deltaVersionUpdates, 0u);
  EXPECT_GT(connectorCalls, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       VoidIndirectCallWithoutValueEdgesStillUpdatesCallGraph) {
  auto module = parseModule(R"(
    @function.pointer = global void ()* @target
    define void @target() {
    entry:
      ret void
    }
    define i32 @main() {
    entry:
      %callee = load void ()*, void ()** @function.pointer
      call void %callee()
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig graphConfig;
  graphConfig.usePointerAnalysis = true;
  graphConfig.buildMSSA = true;
  graphConfig.resolveIndirectCalls = false;
  SVFGBuilder builder(graphConfig);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const Function *mainFunction = module->getFunction("main");
  const Function *targetFunction = module->getFunction("target");
  const CallBase *indirectCall = nullptr;
  for (const Instruction &instruction : instructions(mainFunction)) {
    const auto *call = dyn_cast<CallBase>(&instruction);
    if (call && !call->getCalledFunction())
      indirectCall = call;
  }
  ASSERT_NE(indirectCall, nullptr);

  VersionedFlowSensitivePTA::Config solverConfig;
  solverConfig.connectIndirectCall = [&](const CallBase *callSite,
                                         const Function *target) {
    std::vector<SVFGEdge *> edges;
    return builder.connectCallSiteToCalleeOnTheFly(graph.get(), callSite,
                                                   target, edges);
  };
  VersionedFlowSensitivePTA solver(*graph, std::move(solverConfig));
  const auto &stats = solver.solve();
  EXPECT_NE(graph->getConnectedCallees(indirectCall).count(targetFunction), 0u);
  EXPECT_GT(stats.indirectCallEdges, 0u);
}

TEST_F(VersionedFlowSensitivePTATest,
       MemcpyProducesNewDestinationVersionAndCopiesPointer) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    @y = global i8 0
    define i8* @main() {
    entry:
      %source = alloca %S
      %destination = alloca %S
      %source.field = getelementptr %S, %S* %source, i32 0, i32 0
      %destination.field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* @y, i8** %source.field
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
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  bool hasX = false;
  bool hasY = false;
  for (uint32_t object : *result) {
    hasX |= graph->getObjectValue(object) == module->getGlobalVariable("x");
    hasY |= graph->getObjectValue(object) == module->getGlobalVariable("y");
  }
  EXPECT_FALSE(hasX);
  EXPECT_TRUE(hasY);
}

TEST_F(VersionedFlowSensitivePTATest,
       PartialMemcpyPreservesVersionOutsideCopiedRange) {
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
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *copied = nullptr;
  const LoadInst *preserved = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main"))) {
    const auto *load = dyn_cast<LoadInst>(&instruction);
    if (load && load->getName() == "copied")
      copied = load;
    else if (load && load->getName() == "preserved")
      preserved = load;
  }
  ASSERT_NE(copied, nullptr);
  ASSERT_NE(preserved, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  auto pointsToGlobal = [&](const LoadInst *load, StringRef name) {
    const auto result = solver.pointsTo(load);
    return result &&
           std::any_of(result->begin(), result->end(), [&](uint32_t object) {
             return graph->getObjectValue(object) ==
                    module->getGlobalVariable(name);
           });
  };
  EXPECT_TRUE(pointsToGlobal(copied, "x"));
  EXPECT_FALSE(pointsToGlobal(copied, "z"));
  EXPECT_TRUE(pointsToGlobal(preserved, "w"));
  EXPECT_FALSE(pointsToGlobal(preserved, "y"));
}

TEST_F(VersionedFlowSensitivePTATest, ZeroMemsetProducesEmptyPointerVersion) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %destination = alloca %S
      %field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* @x, i8** %field
      %bytes = bitcast %S* %destination to i8*
      call void @llvm.memset.p0i8.i64(i8* %bytes, i8 0,
                                      i64 8, i1 false)
      %result = load i8*, i8** %field
      ret i8* %result
    }
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->empty());
}

TEST_F(VersionedFlowSensitivePTATest, ZeroLengthMemsetPreservesPointerVersion) {
  auto module = parseModule(R"(
    %S = type { i8* }
    @x = global i8 0
    define i8* @main() {
    entry:
      %destination = alloca %S
      %field = getelementptr %S, %S* %destination, i32 0, i32 0
      store i8* @x, i8** %field
      %bytes = bitcast %S* %destination to i8*
      call void @llvm.memset.p0i8.i64(i8* %bytes, i8 0,
                                      i64 0, i1 false)
      %result = load i8*, i8** %field
      ret i8* %result
    }
    declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.buildMSSA = true;
  SVFGBuilder builder(config);
  std::unique_ptr<SVFG> graph(builder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  const LoadInst *load = nullptr;
  for (const Instruction &instruction :
       instructions(module->getFunction("main")))
    if (const auto *candidate = dyn_cast<LoadInst>(&instruction))
      load = candidate;
  ASSERT_NE(load, nullptr);

  VersionedFlowSensitivePTA solver(*graph);
  solver.solve();
  const auto result = solver.pointsTo(load);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(std::any_of(result->begin(), result->end(), [&](uint32_t object) {
    return graph->getObjectValue(object) == module->getGlobalVariable("x");
  }));
}
