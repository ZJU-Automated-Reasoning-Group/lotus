#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/Thread/ThreadCreationTree.h"
#include "Concurrency/Utils/ThreadAPI.h"
#include "Concurrency/ValueFlow/MultiStageSlicer.h"
#include "Concurrency/ValueFlow/SparseValueFlowRefinement.h"
#include "Concurrency/ValueFlow/ThreadAwareSVFG.h"
#include "IR/ICFG/ICFGBuilder.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

namespace {

class FixedMHP final : public mhp::IMHPAnalysis {
public:
  explicit FixedMHP(bool parallel) : parallel_(parallel) {}

  void analyze() override {}
  bool mayHappenInParallel(const Instruction *,
                           const Instruction *) const override {
    return parallel_;
  }
  bool isPrecomputedMHP(const Instruction *lhs,
                        const Instruction *rhs) const override {
    return mayHappenInParallel(lhs, rhs);
  }
  mhp::InstructionSet
  getParallelInstructions(const Instruction *) const override {
    return {};
  }
  bool mustBeSequential(const Instruction *lhs,
                        const Instruction *rhs) const override {
    return !mayHappenInParallel(lhs, rhs);
  }
  mhp::ThreadID getThreadID(const Instruction *) const override { return 0; }
  mhp::InstructionSet getInstructionsInThread(mhp::ThreadID) const override {
    return {};
  }
  size_t getMhpPairCount() const override { return 0; }
  void printStatistics(raw_ostream &) const override {}
  void printResults(raw_ostream &) const override {}

private:
  bool parallel_;
};

class ValueFlowTest : public LlvmModuleTest {};

TEST_F(ValueFlowTest, ThreadOverlayAddsAndClearsGuardedInterference) {
  auto module = parseModule(R"(
    @cell = global i8 0
    define void @writer() {
    entry:
      store i8 1, i8* @cell
      ret void
    }
    define i8 @reader() {
    entry:
      %value = load i8, i8* @cell
      ret i8 %value
    }
  )");
  ASSERT_NE(module, nullptr);

  StoreInst *store = nullptr;
  LoadInst *load = nullptr;
  for (Instruction &instruction : instructions(module->getFunction("writer")))
    store = dyn_cast<StoreInst>(&instruction) ? cast<StoreInst>(&instruction)
                                              : store;
  for (Instruction &instruction : instructions(module->getFunction("reader")))
    load =
        dyn_cast<LoadInst>(&instruction) ? cast<LoadInst>(&instruction) : load;
  ASSERT_NE(store, nullptr);
  ASSERT_NE(load, nullptr);

  SVFG graph;
  constexpr uint32_t cellObject = 900;
  auto *storeNode = new StoreSVFGNode(1, nullptr, store, 0);
  storeNode->setMemoryDef(1, 1, {cellObject});
  auto *loadNode = new LoadSVFGNode(2, nullptr, load, 0);
  loadNode->setMemoryUse(1, 1, {cellObject});
  auto *excludedLoadNode = new LoadSVFGNode(3, nullptr, load, 0);
  excludedLoadNode->setMemoryUse(1, 1, {cellObject});
  graph.addNode(storeNode);
  graph.addNode(loadNode);
  graph.addNode(excludedLoadNode);
  SVFG::ObjectInfo info;
  info.isGlobal = true;
  graph.setObjectInfo(cellObject, info);

  FixedMHP parallel(true);
  FilteredSVFGView::NodeSet retained{storeNode, loadNode};
  FilteredSVFGView scope(graph, std::move(retained));
  ThreadAwareSVFGBuilder overlay(graph, parallel, nullptr, &scope);
  const auto &stats = overlay.build();
  EXPECT_EQ(stats.edgesAdded, 1u);
  ASSERT_EQ(storeNode->getOutEdges().size(), 1u);
  EXPECT_EQ(storeNode->getOutEdges().front()->getEdgeKind(),
            SVFGEdgeK::ThreadMHPIndirectVF);
  EXPECT_EQ(storeNode->getOutEdges().front()->getPointsTo(),
            SVFGNodeBS({cellObject}));

  overlay.clear();
  EXPECT_TRUE(storeNode->getOutEdges().empty());
  EXPECT_TRUE(loadNode->getInEdges().empty());
  EXPECT_TRUE(excludedLoadNode->getInEdges().empty());
}

TEST_F(ValueFlowTest, ThreadOverlayRejectsSequentialPairs) {
  auto module = parseModule(R"(
    @cell = global i8 0
    define void @f() {
    entry:
      store i8 1, i8* @cell
      %value = load i8, i8* @cell
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);
  Function *function = module->getFunction("f");
  auto it = function->getEntryBlock().begin();
  auto *store = cast<StoreInst>(&*it++);
  auto *load = cast<LoadInst>(&*it);

  SVFG graph;
  auto *storeNode = new StoreSVFGNode(1, nullptr, store, 0);
  storeNode->setMemoryDef(1, 1, {42});
  auto *loadNode = new LoadSVFGNode(2, nullptr, load, 0);
  loadNode->setMemoryUse(1, 1, {42});
  graph.addNode(storeNode);
  graph.addNode(loadNode);

  FixedMHP sequential(false);
  ThreadAwareSVFGBuilder overlay(graph, sequential);
  EXPECT_EQ(overlay.build().edgesAdded, 0u);
  EXPECT_TRUE(storeNode->getOutEdges().empty());
}

TEST_F(ValueFlowTest, LightweightRefinementKillsOverwrittenSingletonGlobal) {
  auto module = parseModule(R"(
    @slot = global i8* null
    @x = global i8 0
    @y = global i8 0
    define i8* @f() {
    entry:
      store i8* @x, i8** @slot
      store i8* @y, i8** @slot
      %result = load i8*, i8** @slot
      %through_result = load i8, i8* %result
      %direct_x = load i8, i8* @x
      ret i8* %result
    }
  )");
  ASSERT_NE(module, nullptr);
  Function *function = module->getFunction("f");
  auto it = function->getEntryBlock().begin();
  auto *firstStore = cast<StoreInst>(&*it++);
  auto *secondStore = cast<StoreInst>(&*it++);
  auto *load = cast<LoadInst>(&*it);
  auto *throughResult = cast<LoadInst>(&*++it);
  auto *directX = cast<LoadInst>(&*++it);

  constexpr uint32_t xObject = 100;
  constexpr uint32_t yObject = 101;
  constexpr uint32_t slotObject = 200;

  SVFG graph;
  auto *x =
      new AddrSVFGNode(1, nullptr, module->getGlobalVariable("x"), xObject);
  auto *y =
      new AddrSVFGNode(2, nullptr, module->getGlobalVariable("y"), yObject);
  auto *storeX = new StoreSVFGNode(3, nullptr, firstStore, 0);
  storeX->setMemoryDef(1, 1, {slotObject});
  auto *storeY = new StoreSVFGNode(4, nullptr, secondStore, 0);
  storeY->setMemoryDef(1, 2, {slotObject});
  auto *result = new LoadSVFGNode(5, nullptr, load, 0);
  result->setMemoryUse(1, 2, {slotObject});

  graph.addNode(x);
  graph.addNode(y);
  graph.addNode(storeX);
  graph.addNode(storeY);
  graph.addNode(result);
  graph.setValueNode(module->getGlobalVariable("x"), x->getId());
  graph.setValueNode(module->getGlobalVariable("y"), y->getId());
  graph.setValueNode(load, result->getId());
  graph.setDef(firstStore, storeX->getId());
  graph.setDef(secondStore, storeY->getId());
  graph.setDef(load, result->getId());

  SVFG::ObjectInfo global;
  global.isGlobal = true;
  global.isSingleton = true;
  graph.setObjectInfo(xObject, global);
  graph.setObjectInfo(yObject, global);
  graph.setObjectInfo(slotObject, global);
  graph.addEdge(storeX, storeY, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});
  graph.addEdge(storeY, result, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});

  SparseValueFlowRefinement solver(graph);
  solver.solve();

  ASSERT_TRUE(solver.hasCompletePointsTo(result));
  EXPECT_EQ(solver.pointsTo(result), SVFGNodeBS({yObject}));
  EXPECT_EQ(solver.pointsTo(result).count(xObject), 0u);
  ASSERT_TRUE(solver.pointsTo(load).has_value());
  EXPECT_EQ(*solver.pointsTo(load), SVFGNodeBS({yObject}));
  ASSERT_TRUE(solver.mayAliasAccesses(throughResult, directX).has_value());
  EXPECT_FALSE(*solver.mayAliasAccesses(throughResult, directX));
  EXPECT_GT(solver.statistics().strongUpdates, 0u);
  EXPECT_EQ(solver.backend(), lotus::alias::PointsToSetBackend::Mutable);
  EXPECT_EQ(solver.statistics().hashConsedUniqueSets, 0u);

  SparseValueFlowRefinement hashConsedSolver(
      graph, nullptr, lotus::alias::PointsToSetBackend::HashConsed);
  hashConsedSolver.solve();
  EXPECT_EQ(hashConsedSolver.pointsTo(result), SVFGNodeBS({yObject}));
  EXPECT_EQ(hashConsedSolver.pointsTo(result), solver.pointsTo(result));
  EXPECT_GT(hashConsedSolver.statistics().hashConsedUniqueSets, 0u);
  EXPECT_GT(hashConsedSolver.statistics().hashConsedUnionRequests, 0u);
}

TEST_F(ValueFlowTest, MultiStageSliceDropsUnrelatedValueFlow) {
  auto module = parseModule(R"(
    @shared = global i8 0
    @other = global i8 0
    define void @writer() {
    entry:
      store i8 1, i8* @shared
      ret void
    }
    define i8 @reader() {
    entry:
      %value = load i8, i8* @shared
      ret i8 %value
    }
    define void @unrelated() {
    entry:
      store i8 2, i8* @other
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  auto findStore = [](llvm::Function *function) {
    for (llvm::Instruction &instruction : llvm::instructions(function))
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
        return store;
    return static_cast<llvm::StoreInst *>(nullptr);
  };
  auto findLoad = [](llvm::Function *function) {
    for (llvm::Instruction &instruction : llvm::instructions(function))
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
        return load;
    return static_cast<llvm::LoadInst *>(nullptr);
  };

  StoreInst *writer = findStore(module->getFunction("writer"));
  LoadInst *reader = findLoad(module->getFunction("reader"));
  StoreInst *unrelated = findStore(module->getFunction("unrelated"));
  ASSERT_NE(writer, nullptr);
  ASSERT_NE(reader, nullptr);
  ASSERT_NE(unrelated, nullptr);

  SVFG graph;
  auto *writerNode = new StoreSVFGNode(1, nullptr, writer, 0);
  writerNode->setMemoryDef(1, 1, {100});
  auto *readerNode = new LoadSVFGNode(2, nullptr, reader, 0);
  readerNode->setMemoryUse(1, 1, {100});
  auto *unrelatedNode = new StoreSVFGNode(3, nullptr, unrelated, 0);
  unrelatedNode->setMemoryDef(2, 1, {200});
  auto *sharedAddress =
      new AddrSVFGNode(4, nullptr, module->getGlobalVariable("shared"), 100);
  auto *otherAddress =
      new AddrSVFGNode(5, nullptr, module->getGlobalVariable("other"), 200);
  graph.addNode(writerNode);
  graph.addNode(readerNode);
  graph.addNode(unrelatedNode);
  graph.addNode(sharedAddress);
  graph.addNode(otherAddress);
  graph.addEdge(sharedAddress, writerNode, SVFGEdgeK::IntraDirect);
  graph.addEdge(writerNode, readerNode, SVFGEdgeK::ThreadMHPIndirectVF, nullptr,
                {100});
  graph.addEdge(otherAddress, unrelatedNode, SVFGEdgeK::IntraDirect);

  MultiStageSlicer slicer(graph);
  std::unique_ptr<FilteredSVFGView> slice = slicer.slice();
  ASSERT_NE(slice, nullptr);
  EXPECT_TRUE(slice->contains(writerNode));
  EXPECT_TRUE(slice->contains(readerNode));
  EXPECT_TRUE(slice->contains(sharedAddress));
  EXPECT_FALSE(slice->contains(unrelatedNode));
  EXPECT_FALSE(slice->contains(otherAddress));
  EXPECT_LT(slice->nodeCount(), graph.getNumNodes());
}

TEST_F(ValueFlowTest, ThreadCreationTreeResolvesForkContextAndJoinHandle) {
  auto module = parseModule(R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8**)
    define i8* @worker(i8* %arg) { ret i8* null }
    define void @spawn(i8* %tid) {
    entry:
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      ret void
    }
    define i32 @main() {
    entry:
      %tid = alloca i8
      call void @spawn(i8* %tid)
      call i32 @pthread_join(i8* %tid, i8** null)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ThreadCreationTree tree(*module, *ThreadAPI::getThreadAPI(), 2);
  ASSERT_EQ(tree.nodes().size(), 2u);
  ASSERT_EQ(tree.forkRelations().size(), 1u);
  EXPECT_EQ(tree.forkRelations().front().target, module->getFunction("worker"));
  EXPECT_EQ(tree.nodes()[1].parent, 0u);
  EXPECT_EQ(tree.nodes()[1].context.size(), 2u);

  const CallBase *join = nullptr;
  for (Instruction &instruction : instructions(module->getFunction("main"))) {
    auto *call = dyn_cast<CallBase>(&instruction);
    if (call && call->getCalledFunction() &&
        call->getCalledFunction()->getName() == "pthread_join")
      join = call;
  }
  ASSERT_NE(join, nullptr);
  ASSERT_EQ(tree.joinedFunctions(join).size(), 1u);
  EXPECT_EQ(tree.joinedFunctions(join).front(), module->getFunction("worker"));
}

TEST_F(ValueFlowTest, ThreadCreationTreeMarksLoopForkAndSupportsSlicedRebuild) {
  auto module = parseModule(R"(
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    define i8* @worker(i8* %arg) { ret i8* null }
    define i32 @main(i1 %again) {
    entry:
      %tid = alloca i8
      br label %loop
    loop:
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* null)
      br i1 %again, label %loop, label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ThreadCreationTree full(*module, *ThreadAPI::getThreadAPI(), 2);
  ASSERT_EQ(full.nodes().size(), 2u);
  EXPECT_TRUE(full.nodes()[1].multiInstance);

  InstructionScope scope;
  for (Instruction &instruction : instructions(module->getFunction("main")))
    if (!isa<CallBase>(instruction))
      scope.insert(&instruction);
  ThreadCreationTree sliced(*module, *ThreadAPI::getThreadAPI(), 2, &scope);
  EXPECT_EQ(sliced.nodes().size(), 1u);
  EXPECT_TRUE(sliced.forkRelations().empty());
}

TEST_F(ValueFlowTest, ThreadCallGraphResolvesTypedIndirectForkTarget) {
  auto module = parseModule(R"(
    @start = global i8* (i8*)* @worker
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    define i8* @worker(i8* %arg) { ret i8* null }
    define i32 @main() {
    entry:
      %tid = alloca i8
      %target = load i8* (i8*)*, i8* (i8*)** @start
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* %target, i8* null)
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ThreadCreationTree tree(*module, *ThreadAPI::getThreadAPI(), 2);
  ASSERT_EQ(tree.forkRelations().size(), 1u);
  EXPECT_EQ(tree.forkRelations().front().target, module->getFunction("worker"));
}

TEST_F(ValueFlowTest, ThreadAwareMemorySSAConnectsForkInputsAndJoinOutputs) {
  auto module = parseModule(R"(
    @shared = global i8* null
    @x = global i8 0
    @y = global i8 0
    declare i32 @pthread_create(i8*, i8*, i8* (i8*)*, i8*)
    declare i32 @pthread_join(i8*, i8**)
    define i8* @worker(i8* %arg) {
    entry:
      %before = load i8*, i8** @shared
      store i8* @y, i8** @shared
      ret i8* null
    }
    define i32 @main() {
    entry:
      %tid = alloca i8
      store i8* @x, i8** @shared
      call i32 @pthread_create(i8* %tid, i8* null,
                               i8* (i8*)* @worker, i8* @x)
      call i32 @pthread_join(i8* %tid, i8** null)
      %after = load i8*, i8** @shared
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder icfgBuilder(&icfg);
  icfgBuilder.build(module.get());
  SVFGBuilderConfig config;
  config.usePointerAnalysis = true;
  config.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;
  SVFGBuilder svfgBuilder(config);
  std::unique_ptr<SVFG> graph(svfgBuilder.build(&icfg));
  ASSERT_NE(graph, nullptr);

  ThreadCreationTree tree(*module, *ThreadAPI::getThreadAPI(), 2);
  FixedMHP parallel(true);
  ThreadAwareSVFGBuilder overlay(*graph, parallel, nullptr, nullptr, &tree);
  const auto &stats = overlay.build();

  EXPECT_GT(stats.forkParameterEdges, 0u);
  EXPECT_GT(stats.forkMemoryEdges, 0u);
  EXPECT_GT(stats.joinMemoryNodes, 0u);
  EXPECT_GT(stats.joinMemoryEdges, 0u);

  const CallBase *join = nullptr;
  const LoadInst *after = nullptr;
  for (Instruction &instruction : instructions(module->getFunction("main"))) {
    if (auto *call = dyn_cast<CallBase>(&instruction)) {
      if (call->getCalledFunction() &&
          call->getCalledFunction()->getName() == "pthread_join")
        join = call;
    }
    if (auto *load = dyn_cast<LoadInst>(&instruction))
      after = load;
  }
  ASSERT_NE(join, nullptr);
  ASSERT_NE(after, nullptr);
  ASSERT_FALSE(graph->getActualOuts(join).empty());
  SVFGNode *afterNode = graph->getDef(after);
  ASSERT_NE(afterNode, nullptr);
  bool sawJoinFlow = false;
  for (SVFGNode *actualOut : graph->getActualOuts(join))
    for (SVFGEdge *edge : actualOut->getOutEdges())
      sawJoinFlow |= edge->getDstNode() == afterNode &&
                     edge->getEdgeKind() == SVFGEdgeK::IntraIndirect;
  EXPECT_TRUE(sawJoinFlow);
}

} // namespace
