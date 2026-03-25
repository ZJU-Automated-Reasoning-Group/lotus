#include <gtest/gtest.h>
#include <Dataflow/IFDS/Clients/IDEExtendedTaintAnalysis.h>
#include <Dataflow/IFDS/Clients/IDEFeatureTaintAnalysis.h>
#include <Dataflow/IFDS/Clients/IDEGeneralizedLCA.h>
#include <Dataflow/IFDS/Clients/IDEInstInteractionAnalysis.h>
#include <Dataflow/IFDS/Clients/IDESecureHeapPropagation.h>
#include <Dataflow/IFDS/Solvers/IDESolver.h>
#include <TestUtils/LLVMHelpers.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace ifds {
namespace {

class IDENewAnalysesTest : public ::testing::Test {
protected:
  void SetUp() override { Ctx = std::make_unique<llvm::LLVMContext>(); }
  std::unique_ptr<llvm::LLVMContext> Ctx;
};

TEST_F(IDENewAnalysesTest, ExtendedTaintMarksSourceCallResultTainted) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @recv()

    define i32 @main() {
    entry:
      %recv_val = call i8* @recv()
      ret i32 0
    }
  )", "ide_ext_taint");
  auto *Recv = M->getFunction("recv");
  auto *CallRecv = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"),
                                             "recv_val"));

  IDEExtendedTaintAnalysis Problem;
  auto SF = Problem.summary_flow(CallRecv, Recv, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallRecv) > 0);

  auto EF = Problem.summary_edge_function(CallRecv, Recv, CallRecv,
                                          Problem.zero_fact(), CallRecv);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, ExtendedTaintValue::Tainted);
}

TEST_F(IDENewAnalysesTest, FeatureTaintAssignsSourceFeatureBit) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @recv()

    define i32 @main() {
    entry:
      %recv_val = call i8* @recv()
      ret i32 0
    }
  )", "ide_feature_taint");
  auto *Recv = M->getFunction("recv");
  auto *CallRecv = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"),
                                             "recv_val"));

  IDEFeatureTaintAnalysis Problem;
  auto SF = Problem.summary_flow(CallRecv, Recv, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallRecv) > 0);

  auto EF = Problem.summary_edge_function(CallRecv, Recv, CallRecv,
                                          Problem.zero_fact(), CallRecv);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, FeatureTaintValue::Features);
  EXPECT_NE(V.mask & (1ull << 0), 0ull);
}

TEST_F(IDENewAnalysesTest, SecureHeapMarksAllocatorResultAllocated) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    declare i8* @malloc(i64)

    define i32 @main() {
    entry:
      %buf = call i8* @malloc(i64 16)
      ret i32 0
    }
  )", "ide_secure_heap");
  auto *Malloc = M->getFunction("malloc");
  auto *CallMalloc = llvm::cast<llvm::CallBase>(
      lotus::unittest::findInstructionByName(*M->getFunction("main"), "buf"));

  IDESecureHeapPropagation Problem;
  auto SF = Problem.summary_flow(CallMalloc, Malloc, Problem.zero_fact());
  EXPECT_TRUE(SF.count(CallMalloc) > 0);

  auto EF = Problem.summary_edge_function(CallMalloc, Malloc, CallMalloc,
                                          Problem.zero_fact(), CallMalloc);
  auto V = EF(Problem.bottom_value());
  EXPECT_EQ(V.kind, SecureHeapValue::Allocated);
}

TEST_F(IDENewAnalysesTest, InstInteractionMarksLoadAsRead) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    define i32 @main(i32* %ptr) {
    entry:
      %loaded = load i32, i32* %ptr
      ret i32 %loaded
    }
  )", "ide_inst_interaction");
  auto *Main = M->getFunction("main");
  auto *Load = lotus::unittest::findInstructionByName(*Main, "loaded");
  auto *Ret = Main->back().getTerminator();

  IDEInstInteractionAnalysis Problem;
  IDESolver<IDEInstInteractionAnalysis> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Load);
  EXPECT_TRUE(V.kind == InstInteractionValue::Read ||
              V.kind == InstInteractionValue::ReadWrite);
}

TEST_F(IDENewAnalysesTest, GeneralizedLCAComputesConstantSet) {
  auto M = lotus::unittest::parseModuleChecked(*Ctx, R"(
    define i32 @main() {
    entry:
      %sum = add i32 3, 4
      ret i32 %sum
    }
  )", "ide_glca");
  auto *Main = M->getFunction("main");
  auto *Add = lotus::unittest::findInstructionByName(*Main, "sum");
  auto *Ret = Main->back().getTerminator();

  IDEGeneralizedLCA Problem;
  IDESolver<IDEGeneralizedLCA> Solver(Problem);
  Solver.solve(*M);

  auto V = Solver.get_value_at(Ret, Add);
  EXPECT_EQ(V.kind, GLCAValue::ConstantSet);
  EXPECT_EQ(V.constants.size(), 1u);
  EXPECT_TRUE(V.constants.count(7) > 0);
}

} // namespace
} // namespace ifds

#ifndef LOTUS_GTEST_NO_MAIN
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
