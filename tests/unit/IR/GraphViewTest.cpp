#include "IR/GraphView.h"

#include "IR/ICFG/ICFGBuilder.h"
#include "TestUtils/LLVMHelpers.h"

#include <gtest/gtest.h>

using namespace llvm;
using namespace lotus::analysis;
using namespace lotus::unittest;

class GraphViewTest : public LlvmModuleTest {};

TEST_F(GraphViewTest, ICFGViewIsAnInducedNonOwningSubgraph) {
  auto module = parseModule(R"(
    define i32 @main(i1 %c) {
    entry:
      br i1 %c, label %left, label %right
    left:
      br label %exit
    right:
      br label %exit
    exit:
      ret i32 0
    }
  )");
  ASSERT_NE(module, nullptr);

  ICFG icfg;
  ICFGBuilder builder(&icfg);
  builder.build(module.get());

  Function *main = module->getFunction("main");
  ASSERT_NE(main, nullptr);
  ICFGNode *entry = icfg.getIntraBlockNode(&main->getEntryBlock());
  ICFGNode *left = icfg.getIntraBlockNode(
      main->getBasicBlockList().getNextNode(main->getEntryBlock()));
  ASSERT_NE(entry, nullptr);
  ASSERT_NE(left, nullptr);

  FilteredICFGView::NodeSet retained{entry, left};
  FilteredICFGView view(icfg, std::move(retained));

  EXPECT_EQ(view.nodeCount(), 2u);
  EXPECT_TRUE(view.contains(entry));
  ASSERT_EQ(view.successors(entry).size(), 1u);
  EXPECT_EQ(view.successors(entry).front(), left);
  EXPECT_EQ(view.predecessors(left).size(), 1u);
  EXPECT_EQ(view.edgeCount(), 1u);
}
