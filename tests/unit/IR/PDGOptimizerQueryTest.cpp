#include "IR/PDG/Analysis/ContextSensitiveSlicing.h"
#include "IR/PDG/Analysis/MotionLegality.h"
#include "IR/PDG/Analysis/SchedulingQuery.h"
#include "IR/PDG/Analysis/ThinSlicing.h"

#include "TestUtils/LLVMHelpers.h"

#include <memory>

using namespace pdg;
using namespace lotus::unittest;

namespace {

class TestGraph final : public GenericGraph {
public:
  void build(llvm::Module &M) override { (void)M; }
};

class PDGOptimizerQueryTest : public ::testing::Test {
protected:
  Node *addNode(GraphNodeType type = GraphNodeType::INST_OTHER) {
    nodes.emplace_back(std::make_unique<Node>(type));
    Node *n = nodes.back().get();
    graph.addNode(*n);
    return n;
  }

  void addEdge(Node *src, Node *dst, EdgeType type) {
    ASSERT_NE(src, nullptr);
    ASSERT_NE(dst, nullptr);
    edges.emplace_back(std::make_unique<Edge>(src, dst, type));
    Edge *e = edges.back().get();
    src->addOutEdge(*e);
    dst->addInEdge(*e);
    graph.addEdge(*e);
  }

  bool loadModule(const char *ir) {
    context = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic err;
    module = llvm::parseIR(llvm::MemoryBuffer::getMemBuffer(ir)->getMemBufferRef(),
                           err, *context);
    return module != nullptr;
  }

  Node *addValueNode(llvm::Value &value,
                     GraphNodeType type = GraphNodeType::INST_OTHER) {
    nodes.emplace_back(std::make_unique<Node>(value, type));
    Node *n = nodes.back().get();
    graph.addNode(*n);
    return n;
  }

  TestGraph graph;
  std::unique_ptr<llvm::LLVMContext> context;
  std::unique_ptr<llvm::Module> module;
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<std::unique_ptr<Edge>> edges;
};

} // namespace

TEST_F(PDGOptimizerQueryTest, MotionEarlierBlockedByDependence) {
  Node *anchor = addNode();
  Node *moving = addNode();
  addEdge(anchor, moving, EdgeType::DATA_DEF_USE);

  MotionLegalityQuery q(graph);
  auto result = q.canMoveEarlier(*moving, *anchor);

  EXPECT_FALSE(result.legal);
  EXPECT_FALSE(result.blocking_path.empty());
}

TEST_F(PDGOptimizerQueryTest, MotionLaterBlockedByDependence) {
  Node *moving = addNode();
  Node *anchor = addNode();
  addEdge(moving, anchor, EdgeType::DATA_RAW);

  MotionLegalityQuery q(graph);
  auto result = q.canMoveLater(*moving, *anchor);

  EXPECT_FALSE(result.legal);
  EXPECT_FALSE(result.blocking_path.empty());
}

TEST_F(PDGOptimizerQueryTest, MotionAllowedWhenIndependent) {
  Node *anchor = addNode();
  Node *moving = addNode();

  MotionLegalityQuery q(graph);
  auto result = q.canMoveEarlier(*moving, *anchor);

  EXPECT_TRUE(result.legal);
}

TEST_F(PDGOptimizerQueryTest, SchedulingIndependenceWitnesses) {
  Node *a = addNode();
  Node *b = addNode();
  Node *mid = addNode();
  addEdge(a, mid, EdgeType::DATA_DEF_USE);
  addEdge(mid, b, EdgeType::DATA_DEF_USE);

  SchedulingQuery sq(graph);
  auto dep = sq.independent(*a, *b);
  auto indep = sq.independent(*b, *a);

  EXPECT_FALSE(dep.independent);
  EXPECT_FALSE(dep.witness_path_ab.empty());
  EXPECT_FALSE(indep.independent);
  EXPECT_FALSE(indep.witness_path_ba.empty());
}

TEST_F(PDGOptimizerQueryTest, SchedulingReadySetAndLevels) {
  Node *a = addNode();
  Node *b = addNode();
  Node *c = addNode();
  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(a, c, EdgeType::DATA_DEF_USE);

  SchedulingQuery::NodeSet region = {a, b, c};
  SchedulingQuery::NodeSet scheduled;

  SchedulingQuery sq(graph);
  auto ready0 = sq.readySet(region, scheduled);
  EXPECT_EQ(ready0.size(), 1u);
  EXPECT_TRUE(ready0.count(a));

  scheduled.insert(a);
  auto ready1 = sq.readySet(region, scheduled);
  EXPECT_EQ(ready1.size(), 2u);
  EXPECT_TRUE(ready1.count(b));
  EXPECT_TRUE(ready1.count(c));

  auto levels = sq.topologicalLevels(region);
  ASSERT_EQ(levels.size(), 2u);
  EXPECT_EQ(levels[0].size(), 1u);
  EXPECT_EQ(levels[1].size(), 2u);
}

TEST_F(PDGOptimizerQueryTest, SchedulingCriticalPathAndSCC) {
  Node *a = addNode();
  Node *b = addNode();
  Node *c = addNode();
  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(b, c, EdgeType::DATA_DEF_USE);

  SchedulingQuery::NodeSet region = {a, b, c};
  SchedulingQuery sq(graph);

  EXPECT_EQ(sq.criticalPathLength(region), 2u);

  addEdge(c, b, EdgeType::DATA_DEF_USE);
  auto sccs = sq.stronglyConnectedComponents(region);
  EXPECT_GE(sccs.size(), 2u);
}

TEST_F(PDGOptimizerQueryTest, ContextSensitiveForwardSliceMatchesCallReturn) {
  Node *call_a = addNode(GraphNodeType::INST_FUNCALL);
  Node *call_b = addNode(GraphNodeType::INST_FUNCALL);
  Node *entry = addNode(GraphNodeType::FUNC_ENTRY);
  Node *body = addNode();
  Node *ret = addNode(GraphNodeType::INST_RET);

  addEdge(call_a, entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(call_b, entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(entry, body, EdgeType::DATA_DEF_USE);
  addEdge(body, ret, EdgeType::DATA_DEF_USE);
  addEdge(ret, call_a, EdgeType::CONTROLDEP_CALLRET);
  addEdge(ret, call_b, EdgeType::CONTROLDEP_CALLRET);

  ContextSensitiveSlicing slicer(graph);
  auto slice = slicer.computeForwardSlice(
      *call_a, {EdgeType::CONTROLDEP_CALLINV, EdgeType::CONTROLDEP_CALLRET,
                EdgeType::DATA_DEF_USE});

  EXPECT_EQ(slice.size(), 4u);
  EXPECT_TRUE(slice.count(call_a));
  EXPECT_TRUE(slice.count(entry));
  EXPECT_TRUE(slice.count(body));
  EXPECT_TRUE(slice.count(ret));
  EXPECT_FALSE(slice.count(call_b));
}

TEST_F(PDGOptimizerQueryTest,
       ContextSensitiveBackwardSliceRejectsMismatchedCaller) {
  Node *call_a = addNode(GraphNodeType::INST_FUNCALL);
  Node *call_b = addNode(GraphNodeType::INST_FUNCALL);
  Node *entry = addNode(GraphNodeType::FUNC_ENTRY);
  Node *body = addNode();
  Node *ret = addNode(GraphNodeType::INST_RET);

  addEdge(call_a, entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(call_b, entry, EdgeType::CONTROLDEP_CALLINV);
  addEdge(entry, body, EdgeType::DATA_DEF_USE);
  addEdge(body, ret, EdgeType::DATA_DEF_USE);
  addEdge(ret, call_a, EdgeType::CONTROLDEP_CALLRET);
  addEdge(ret, call_b, EdgeType::CONTROLDEP_CALLRET);

  ContextSensitiveSlicing slicer(graph);
  SliceOptions options;
  options.include_data_deps = true;
  options.include_control_deps = false;
  options.include_param_edges = false;
  options.include_call_return_edges = true;
  options.use_summary_cache = true;

  auto slice = slicer.computeBackwardSlice({call_a}, options);

  EXPECT_EQ(slice.size(), 4u);
  EXPECT_TRUE(slice.count(call_a));
  EXPECT_TRUE(slice.count(entry));
  EXPECT_TRUE(slice.count(body));
  EXPECT_TRUE(slice.count(ret));
  EXPECT_FALSE(slice.count(call_b));
}

TEST_F(PDGOptimizerQueryTest, ThinSliceExcludesPointerFlowForStoreAndLoad) {
  constexpr const char *IR = R"(
    define void @f(i32* %p, i32 %v) {
    entry:
      %ptr = getelementptr i32, i32* %p, i64 0
      store i32 %v, i32* %ptr
      %load = load i32, i32* %ptr
      ret void
    }
  )";

  ASSERT_TRUE(loadModule(IR));
  auto *func = module->getFunction("f");
  ASSERT_NE(func, nullptr);

  auto *arg_it = func->arg_begin();
  llvm::Argument &ptr_arg = *arg_it++;
  llvm::Argument &val_arg = *arg_it;
  auto *gep = findInstruction<llvm::GetElementPtrInst>(*func, "ptr");
  auto *store = findInstruction<llvm::StoreInst>(*func);
  auto *load = findInstruction<llvm::LoadInst>(*func, "load");
  ASSERT_NE(gep, nullptr);
  ASSERT_NE(store, nullptr);
  ASSERT_NE(load, nullptr);

  Node *ptr_node = addValueNode(ptr_arg, GraphNodeType::VAR_OTHER);
  Node *val_node = addValueNode(val_arg, GraphNodeType::VAR_OTHER);
  Node *gep_node = addValueNode(*gep);
  Node *store_node = addValueNode(*store);
  Node *load_node = addValueNode(*load);

  addEdge(ptr_node, gep_node, EdgeType::DATA_DEF_USE);
  addEdge(gep_node, store_node, EdgeType::DATA_DEF_USE);
  addEdge(val_node, store_node, EdgeType::DATA_DEF_USE);
  addEdge(store_node, load_node, EdgeType::DATA_ALIAS);
  addEdge(gep_node, load_node, EdgeType::DATA_DEF_USE);

  ThinSlicing thin(graph);
  auto backward = thin.computeBackwardSlice(*load_node);
  auto forward_ptr = thin.computeForwardSlice(*ptr_node);

  EXPECT_EQ(backward.size(), 3u);
  EXPECT_TRUE(backward.count(load_node));
  EXPECT_TRUE(backward.count(store_node));
  EXPECT_TRUE(backward.count(val_node));
  EXPECT_FALSE(backward.count(gep_node));
  EXPECT_FALSE(backward.count(ptr_node));

  EXPECT_EQ(forward_ptr.size(), 1u);
  EXPECT_TRUE(forward_ptr.count(ptr_node));
}

TEST_F(PDGOptimizerQueryTest, SchedulingLevelsRespectSccCondensationOrder) {
  Node *a = addNode();
  Node *b = addNode();
  Node *c = addNode();
  Node *d = addNode();

  addEdge(a, b, EdgeType::DATA_DEF_USE);
  addEdge(b, a, EdgeType::DATA_DEF_USE);
  addEdge(b, c, EdgeType::DATA_DEF_USE);
  addEdge(c, d, EdgeType::DATA_DEF_USE);
  addEdge(d, c, EdgeType::DATA_DEF_USE);

  SchedulingQuery sq(graph);
  SchedulingQuery::NodeSet region = {a, b, c, d};
  auto levels = sq.topologicalLevels(region);

  ASSERT_EQ(levels.size(), 2u);
  EXPECT_EQ(levels[0].size(), 2u);
  EXPECT_EQ(levels[1].size(), 2u);
  EXPECT_TRUE(levels[0].count(a));
  EXPECT_TRUE(levels[0].count(b));
  EXPECT_TRUE(levels[1].count(c));
  EXPECT_TRUE(levels[1].count(d));
}

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
