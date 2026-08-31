#include "Concurrency/MHP/IMHPAnalysis.h"
#include "Concurrency/ValueFlow/SparseFlowSensitivePTA.h"
#include "Concurrency/ValueFlow/ThreadAwareSVFG.h"
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

TEST_F(ValueFlowTest, SparseSolverKillsOverwrittenSingletonGlobalValue) {
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
  graph.setObjectInfo(xObject, global);
  graph.setObjectInfo(yObject, global);
  graph.setObjectInfo(slotObject, global);
  graph.addEdge(storeX, storeY, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});
  graph.addEdge(storeY, result, SVFGEdgeK::IntraIndirect, nullptr,
                {slotObject});

  SparseFlowSensitivePTA solver(graph);
  solver.solve();

  ASSERT_TRUE(solver.hasCompletePointsTo(result));
  EXPECT_EQ(solver.pointsTo(result), SVFGNodeBS({yObject}));
  EXPECT_EQ(solver.pointsTo(result).count(xObject), 0u);
  ASSERT_TRUE(solver.pointsTo(load).has_value());
  EXPECT_EQ(*solver.pointsTo(load), SVFGNodeBS({yObject}));
  ASSERT_TRUE(solver.mayAliasAccesses(throughResult, directX).has_value());
  EXPECT_FALSE(*solver.mayAliasAccesses(throughResult, directX));
  EXPECT_GT(solver.statistics().strongUpdates, 0u);
}

} // namespace
