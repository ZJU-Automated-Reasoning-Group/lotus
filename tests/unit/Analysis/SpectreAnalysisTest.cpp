#include "Analysis/Spectre/CacheSpecuAnalysis.h"
#include "TestUtils/LLVMHelpers.h"

#include <llvm/IR/LLVMContext.h>
#include <gtest/gtest.h>

using namespace lotus::unittest;

namespace {

using spectre::CacheSpecuAnalysis;
using spectre::SpectreAnalysisResult;

SpectreAnalysisResult analyzeModule(llvm::Module &module,
                                    llvm::StringRef functionName,
                                    unsigned depth = 4) {
  auto *function = module.getFunction(functionName);
  EXPECT_NE(function, nullptr);
  llvm::DominatorTree dt(*function);
  llvm::PostDominatorTree pdt(*function);
  CacheSpecuAnalysis analysis(*function, dt, pdt, nullptr, 16, 16, 1, depth, 0);
  EXPECT_TRUE(analysis.SpecuSim(&function->getEntryBlock(), nullptr, nullptr));
  return analysis.getResult();
}

TEST(SpectreAnalysisTest, StraightLineCodeHasNoFindings) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    @data = global i32 0, align 4

    define i32 @example() {
    entry:
      %value = load i32, i32* @data, align 4
      ret i32 %value
    }
  )", "SpectreAnalysisTest");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  EXPECT_FALSE(result.hasFindings());
}

TEST(SpectreAnalysisTest, ReportsBranchDependentCacheDivergence) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    @public = global i32 0, align 4
    @secret = global i32 0, align 4

    define i32 @example(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      %lhs = load i32, i32* @secret, align 4
      br label %merge

    else:
      %rhs = load i32, i32* @public, align 4
      br label %merge

    merge:
      %phi = phi i32 [ %lhs, %then ], [ %rhs, %else ]
      ret i32 %phi
    }
  )", "SpectreAnalysisTest");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  ASSERT_TRUE(result.hasFindings());
  EXPECT_EQ(result.Findings.size(), 1u);
  ASSERT_FALSE(result.Findings.front().ThenObservations.empty());
  EXPECT_TRUE(result.Findings.front().HasDivergence);
}

TEST(SpectreAnalysisTest, ReconvergingBranchProducesSingleFinding) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    @a = global i32 0, align 4
    @b = global i32 0, align 4

    define void @example(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      %x = load i32, i32* @a, align 4
      br label %merge

    else:
      %y = load i32, i32* @b, align 4
      br label %merge

    merge:
      store i32 0, i32* @a, align 4
      ret void
    }
  )", "SpectreAnalysisTest");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  ASSERT_TRUE(result.hasFindings());
  EXPECT_EQ(result.Findings.size(), 1u);
}

TEST(SpectreAnalysisTest, LoopingSpeculationStillConverges) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    @arr = global [4 x i32] zeroinitializer, align 16

    define void @example(i1 %cond, i32 %n) {
    entry:
      br i1 %cond, label %loop, label %exit

    loop:
      %i = phi i32 [ 0, %entry ], [ %next, %body ]
      br label %body

    body:
      %ptr = getelementptr inbounds [4 x i32], [4 x i32]* @arr, i64 0, i32 0
      %v = load i32, i32* %ptr, align 4
      %next = add i32 %i, 1
      %keep = icmp slt i32 %next, %n
      br i1 %keep, label %loop, label %exit

    exit:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example", 3);
  EXPECT_GE(result.ArchitecturalMisses, 0u);
}

TEST(SpectreAnalysisTest, UnknownCallIsModeledConservatively) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @opaque(i8*)
    @buf = global [4 x i8] zeroinitializer, align 4

    define void @example(i1 %cond) {
    entry:
      %ptr = getelementptr inbounds [4 x i8], [4 x i8]* @buf, i64 0, i64 0
      br i1 %cond, label %then, label %else

    then:
      call void @opaque(i8* %ptr)
      br label %merge

    else:
      br label %merge

    merge:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  ASSERT_TRUE(result.hasFindings());
  EXPECT_TRUE(result.Findings.front().ThenObservations.front().FromCall);
}

TEST(SpectreAnalysisTest, StackAndHeapObjectsParticipateInModel) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare i8* @malloc(i64)

    define void @example(i1 %cond) {
    entry:
      %stack = alloca i32, align 4
      %heap = call i8* @malloc(i64 8)
      br i1 %cond, label %then, label %else

    then:
      %heap.i32 = bitcast i8* %heap to i32*
      store i32 1, i32* %stack, align 4
      %a = load i32, i32* %heap.i32, align 4
      br label %merge

    else:
      br label %merge

    merge:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  ASSERT_TRUE(result.hasFindings());
  EXPECT_FALSE(result.Findings.front().ThenObservations.empty());
}

TEST(SpectreAnalysisTest, FenceStopsSpeculativeObservation) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    declare void @llvm.x86.sse2.lfence()
    @secret = global i32 0, align 4

    define void @example(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      call void @llvm.x86.sse2.lfence()
      %x = load i32, i32* @secret, align 4
      br label %merge

    else:
      br label %merge

    merge:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  EXPECT_FALSE(result.hasFindings());
}

TEST(SpectreAnalysisTest, SharedFootprintDoesNotReportFinding) {
  llvm::LLVMContext context;
  auto module = parseModule(context, R"(
    @shared = global i32 0, align 4

    define void @example(i1 %cond) {
    entry:
      br i1 %cond, label %then, label %else

    then:
      %a = load i32, i32* @shared, align 4
      br label %merge

    else:
      %b = load i32, i32* @shared, align 4
      br label %merge

    merge:
      ret void
    }
  )");
  ASSERT_NE(module, nullptr);

  SpectreAnalysisResult result = analyzeModule(*module, "example");
  EXPECT_FALSE(result.hasFindings());
}

} // namespace
