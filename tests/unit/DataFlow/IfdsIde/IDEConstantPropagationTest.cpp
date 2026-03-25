#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IDEConstantPropagation.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>
#include <TestUtils/LLVMHelpers.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace ifds {
namespace {

class IDEConstantPropagationTest : public ::testing::Test {
protected:
  void SetUp() override { Ctx = std::make_unique<llvm::LLVMContext>(); }

  std::unique_ptr<llvm::LLVMContext> Ctx;
};

TEST_F(IDEConstantPropagationTest, ComputeConstFromTwoConsts) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    define i32 @main() {
    entry:
      %a = alloca i32
      %b = alloca i32
      store i32 1, i32* %a
      store i32 2, i32* %b
      %l1 = load i32, i32* %a
      %l2 = load i32, i32* %b
      %sum = add i32 %l1, %l2
      ret i32 %sum
    }
  )", "lcp_two_consts");
  auto *Main = M->getFunction("main");
  auto *Add = lotus::unittest::findInstructionByName(*Main, "sum");
  auto *Ret = Main->back().getTerminator();

  IDEConstantPropagation Problem;
  IDESolver<IDEConstantPropagation> Solver(Problem);
  Solver.solve(*M);

  // Values for newly-created facts are typically available at successor nodes.
  auto V = Solver.get_value_at(Ret, Add);
  EXPECT_EQ(V.kind, LCPValue::Const);
  EXPECT_EQ(V.value, 3);
}

TEST_F(IDEConstantPropagationTest, PropagateThroughStoreLoadAndBinop) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    define i32 @main() {
    entry:
      %x = alloca i32
      store i32 5, i32* %x
      %lx = load i32, i32* %x
      %plus2 = add i32 %lx, 2
      ret i32 %plus2
    }
  )", "lcp_store_load");
  auto *Main = M->getFunction("main");
  auto *LoadX = lotus::unittest::findInstructionByName(*Main, "lx");
  auto *Add = lotus::unittest::findInstructionByName(*Main, "plus2");
  auto *Ret = Main->back().getTerminator();

  IDEConstantPropagation Problem;
  IDESolver<IDEConstantPropagation> Solver(Problem);
  Solver.solve(*M);

  // Check load value at return (successor of all computations).
  auto LoadVal = Solver.get_value_at(Ret, LoadX);
  EXPECT_EQ(LoadVal.kind, LCPValue::Const);
  EXPECT_EQ(LoadVal.value, 5);

  // Check add result value at return.
  auto AddVal = Solver.get_value_at(Ret, Add);
  EXPECT_EQ(AddVal.kind, LCPValue::Const);
  EXPECT_EQ(AddVal.value, 7);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
